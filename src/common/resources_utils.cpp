// Licensed to the Apache Software Foundation (ASF) under one
// or more contributor license agreements.  See the NOTICE file
// distributed with this work for additional information
// regarding copyright ownership.  The ASF licenses this file
// to you under the Apache License, Version 2.0 (the
// "License"); you may not use this file except in compliance
// with the License.  You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#include <stout/foreach.hpp>
#include <stout/stringify.hpp>

#include "common/resources_utils.hpp"

using std::vector;

using google::protobuf::RepeatedPtrField;

namespace mesos {

bool needCheckpointing(const Resource& resource)
{
  return !Resources::hasResourceProvider(resource) &&
         (Resources::isDynamicallyReserved(resource) ||
          Resources::isPersistentVolume(resource));
}


// NOTE: We effectively duplicate the logic in 'Resources::apply'
// which is less than ideal. But we cannot simply create
// 'Offer::Operation' and invoke 'Resources::apply' here.
// 'RESERVE' operation requires that the specified resources are
// dynamically reserved only, and 'CREATE' requires that the
// specified resources are already dynamically reserved.
// These requirements are violated when we try to infer dynamically
// reserved persistent volumes.
// TODO(mpark): Consider introducing an atomic 'RESERVE_AND_CREATE'
// operation to solve this problem.
Try<Resources> applyCheckpointedResources(
    const Resources& resources,
    const Resources& checkpointedResources)
{
  Resources totalResources = resources;

  foreach (const Resource& resource, checkpointedResources) {
    if (!needCheckpointing(resource)) {
      return Error("Unexpected checkpointed resources " + stringify(resource));
    }

    Resource stripped = resource;

    // Since only unreserved and statically reserved resources can be specified
    // on the agent, we strip away all of the dynamic reservations here to
    // deduce the agent resources on which to apply the checkpointed resources.
    if (Resources::isDynamicallyReserved(resource)) {
      Resource::ReservationInfo reservation = stripped.reservations(0);
      stripped.clear_reservations();
      if (reservation.type() == Resource::ReservationInfo::STATIC) {
        stripped.add_reservations()->CopyFrom(reservation);
      }
    }

    // Strip persistence and volume from the disk info so that we can
    // check whether it is contained in the `totalResources`.
    if (Resources::isPersistentVolume(resource)) {
      if (stripped.disk().has_source()) {
        stripped.mutable_disk()->clear_persistence();
        stripped.mutable_disk()->clear_volume();
      } else {
        stripped.clear_disk();
      }
    }

    stripped.clear_shared();

    if (!totalResources.contains(stripped)) {
      return Error(
          "Incompatible agent resources: " + stringify(totalResources) +
          " does not contain " + stringify(stripped));
    }

    totalResources -= stripped;
    totalResources += resource;
  }

  return totalResources;
}


namespace internal {

// NOTE: Use template here so that it works for both internal and v1.
template <typename TResources,
          typename TResource,
          typename TResourceConversion,
          typename TOperation>
Try<vector<TResourceConversion>> getResourceConversions(
    const TOperation& operation)
{
  vector<TResourceConversion> conversions;

  switch (operation.type()) {
    case TOperation::UNKNOWN:
      return Error("Unknown operation");

    case TOperation::LAUNCH:
    case TOperation::LAUNCH_GROUP:
    case TOperation::CREATE_VOLUME:
    case TOperation::DESTROY_VOLUME:
    case TOperation::CREATE_BLOCK:
    case TOperation::DESTROY_BLOCK:
      return Error("Operation not supported");

    case TOperation::RESERVE: {
      foreach (const TResource& reserved, operation.reserve().resources()) {
        // Note that we only allow "pushing" a single reservation at time.
        TResources consumed = TResources(reserved).popReservation();
        conversions.emplace_back(consumed, reserved);
      }
      break;
    }

    case TOperation::UNRESERVE: {
      foreach (const TResource& reserved, operation.unreserve().resources()) {
        // Note that we only allow "popping" a single reservation at time.
        TResources converted = TResources(reserved).popReservation();
        conversions.emplace_back(reserved, converted);
      }
      break;
    }

    case TOperation::CREATE: {
      foreach (const TResource& volume, operation.create().volumes()) {
        // Strip persistence and volume from the disk info so that we
        // can subtract it from the original resources.
        // TODO(jieyu): Non-persistent volumes are not supported for
        // now. Persistent volumes can only be be created from regular
        // disk resources. Revisit this once we start to support
        // non-persistent volumes.
        TResource stripped = volume;

        if (stripped.disk().has_source()) {
          stripped.mutable_disk()->clear_persistence();
          stripped.mutable_disk()->clear_volume();
        } else {
          stripped.clear_disk();
        }

        // Since we only allow persistent volumes to be shared, the
        // original resource must be non-shared.
        stripped.clear_shared();

        conversions.emplace_back(stripped, volume);
      }
      break;
    }

    case TOperation::DESTROY: {
      foreach (const TResource& volume, operation.destroy().volumes()) {
        // Strip persistence and volume from the disk info so that we
        // can subtract it from the original resources.
        TResource stripped = volume;

        if (stripped.disk().has_source()) {
          stripped.mutable_disk()->clear_persistence();
          stripped.mutable_disk()->clear_volume();
        } else {
          stripped.clear_disk();
        }

        // Since we only allow persistent volumes to be shared, we
        // return the resource to non-shared state after destroy.
        stripped.clear_shared();

        conversions.emplace_back(
            volume,
            stripped,
            [volume](const TResources& resources) -> Try<Nothing> {
              if (resources.contains(volume)) {
                return Error(
                  "Persistent volume " + stringify(volume) + " cannot be "
                  "removed due to additional shared copies");
              }
              return Nothing();
            });
      }
      break;
    }
  }

  return conversions;
}

} // namespace internal {


Try<vector<ResourceConversion>> getResourceConversions(
    const Offer::Operation& operation)
{
  return internal::getResourceConversions<
      Resources,
      Resource,
      ResourceConversion,
      Offer::Operation>(operation);
}


Try<vector<v1::ResourceConversion>> getResourceConversions(
    const v1::Offer::Operation& operation)
{
  return internal::getResourceConversions<
      v1::Resources,
      v1::Resource,
      v1::ResourceConversion,
      v1::Offer::Operation>(operation);
}


Result<ResourceProviderID> getResourceProviderId(
    const Offer::Operation& operation)
{
  Option<Resource> resource;

  switch (operation.type()) {
    case Offer::Operation::LAUNCH:
      return Error("Unexpected LAUNCH operation");
    case Offer::Operation::LAUNCH_GROUP:
      return Error("Unexpected LAUNCH_GROUP operation");
    case Offer::Operation::RESERVE:
      if (operation.reserve().resources().empty()) {
        return Error("Operation contains no resources");
      }
      resource = operation.reserve().resources(0);
      break;
    case Offer::Operation::UNRESERVE:
      if (operation.unreserve().resources().empty()) {
        return Error("Operation contains no resources");
      }
      resource = operation.unreserve().resources(0);
      break;
    case Offer::Operation::CREATE:
      if (operation.create().volumes().empty()) {
        return Error("Operation contains no resources");
      }
      resource = operation.create().volumes(0);
      break;
    case Offer::Operation::DESTROY:
      if (operation.destroy().volumes().empty()) {
        return Error("Operation contains no resources");
      }
      resource = operation.destroy().volumes(0);
      break;
    case Offer::Operation::CREATE_VOLUME:
      resource = operation.create_volume().source();
      break;
    case Offer::Operation::DESTROY_VOLUME:
      resource = operation.destroy_volume().volume();
      break;
    case Offer::Operation::CREATE_BLOCK:
      resource = operation.create_block().source();
      break;
    case Offer::Operation::DESTROY_BLOCK:
      resource = operation.destroy_block().block();
      break;
    case Offer::Operation::UNKNOWN:
      return Error("Unknown offer operation");
  }

  CHECK_SOME(resource);

  if (resource->has_provider_id()) {
    return resource->provider_id();
  }

  return None();
}


void convertResourceFormat(Resource* resource, ResourceFormat format)
{
  switch (format) {
    case PRE_RESERVATION_REFINEMENT:
    case ENDPOINT: {
      CHECK(!resource->has_role());
      CHECK(!resource->has_reservation());
      switch (resource->reservations_size()) {
        // Unreserved resource.
        case 0: {
          resource->set_role("*");
          break;
        }
        // Resource with a single reservation.
        case 1: {
          const Resource::ReservationInfo& source = resource->reservations(0);

          if (source.type() == Resource::ReservationInfo::DYNAMIC) {
            Resource::ReservationInfo* target = resource->mutable_reservation();
            if (source.has_principal()) {
              target->set_principal(source.principal());
            }
            if (source.has_labels()) {
              target->mutable_labels()->CopyFrom(source.labels());
            }
          }

          resource->set_role(source.role());

          if (format == PRE_RESERVATION_REFINEMENT) {
            resource->clear_reservations();
          }
          break;
        }
        // Resource with refined reservations.
        default: {
          CHECK_NE(PRE_RESERVATION_REFINEMENT, format)
            << "Invalid resource format conversion: A 'Resource' object"
               " being converted to the PRE_RESERVATION_REFINEMENT format"
               " must not have refined reservations";
        }
      }
      break;
    }
    case POST_RESERVATION_REFINEMENT: {
      if (resource->reservations_size() > 0) {
        // In this case, we're either already in
        // the "post-reservation-refinement" format,
        // or we're in the "endpoint" format.

        // We clear out the "pre-reservation-refinement" fields
        // in case the resources are in the "endpoint" format.
        resource->clear_role();
        resource->clear_reservation();
        return;
      }

      // Unreserved resources.
      if (resource->role() == "*") {
        CHECK(!resource->has_reservation());
        resource->clear_role();
        return;
      }

      // Resource with a single reservation.
      Resource::ReservationInfo* reservation = resource->add_reservations();

      // Check the `Resource.reservation` to determine whether
      // we have a static or dynamic reservation.
      if (!resource->has_reservation()) {
        reservation->set_type(Resource::ReservationInfo::STATIC);
      } else {
        reservation->CopyFrom(resource->reservation());
        resource->clear_reservation();
        reservation->set_type(Resource::ReservationInfo::DYNAMIC);
      }

      reservation->set_role(resource->role());
      resource->clear_role();
      break;
    }
  }
}


void convertResourceFormat(
    RepeatedPtrField<Resource>* resources,
    ResourceFormat format)
{
  foreach (Resource& resource, *resources) {
    convertResourceFormat(&resource, format);
  }
}


void convertResourceFormat(
    std::vector<Resource>* resources,
    ResourceFormat format)
{
  foreach (Resource& resource, *resources) {
    convertResourceFormat(&resource, format);
  }
}


Option<Error> validateAndNormalizeResources(Offer::Operation* operation)
{
  CHECK_NOTNULL(operation);

  switch (operation->type()) {
    case Offer::Operation::RESERVE: {
      // TODO(mpark): Once we perform a sanity check validation for
      // offer operations as specified in MESOS-7760, this should no
      // longer have to be handled in this function.
      if (!operation->has_reserve()) {
        return Error(
            "A RESERVE offer operation must have"
            " the Offer.Operation.reserve field set.");
      }

      Option<Error> error =
        Resources::validate(operation->reserve().resources());

      if (error.isSome()) {
        return error;
      }

      convertResourceFormat(
          operation->mutable_reserve()->mutable_resources(),
          POST_RESERVATION_REFINEMENT);

      return None();
    }
    case Offer::Operation::UNRESERVE: {
      // TODO(mpark): Once we perform a sanity check validation for
      // offer operations as specified in MESOS-7760, this should no
      // longer have to be handled in this function.
      if (!operation->has_unreserve()) {
        return Error(
            "An UNRESERVE offer operation must have"
            " the Offer.Operation.unreserve field set.");
      }

      Option<Error> error =
        Resources::validate(operation->unreserve().resources());

      if (error.isSome()) {
        return error;
      }

      convertResourceFormat(
          operation->mutable_unreserve()->mutable_resources(),
          POST_RESERVATION_REFINEMENT);

      return None();
    }
    case Offer::Operation::CREATE: {
      // TODO(mpark): Once we perform a sanity check validation for
      // offer operations as specified in MESOS-7760, this should no
      // longer have to be handled in this function.
      if (!operation->has_create()) {
        return Error(
            "A CREATE offer operation must have"
            " the Offer.Operation.create field set.");
      }

      Option<Error> error =
        Resources::validate(operation->create().volumes());

      if (error.isSome()) {
        return error;
      }

      convertResourceFormat(
          operation->mutable_create()->mutable_volumes(),
          POST_RESERVATION_REFINEMENT);

      return None();
    }
    case Offer::Operation::DESTROY: {
      // TODO(mpark): Once we perform a sanity check validation for
      // offer operations as specified in MESOS-7760, this should no
      // longer have to be handled in this function.
      if (!operation->has_destroy()) {
        return Error(
            "A DESTROY offer operation must have"
            " the Offer.Operation.destroy field set.");
      }

      Option<Error> error =
        Resources::validate(operation->destroy().volumes());

      if (error.isSome()) {
        return error;
      }

      convertResourceFormat(
          operation->mutable_destroy()->mutable_volumes(),
          POST_RESERVATION_REFINEMENT);

      return None();
    }
    case Offer::Operation::LAUNCH: {
      // TODO(mpark): Once we perform a sanity check validation for
      // offer operations as specified in MESOS-7760, this should no
      // longer have to be handled in this function.
      if (!operation->has_launch()) {
        return Error(
            "A LAUNCH offer operation must have"
            " the Offer.Operation.launch field set.");
      }

      // Validate resources in LAUNCH.
      foreach (const TaskInfo& task, operation->launch().task_infos()) {
        Option<Error> error = Resources::validate(task.resources());
        if (error.isSome()) {
          return error;
        }

        if (task.has_executor()) {
          Option<Error> error =
            Resources::validate(task.executor().resources());

          if (error.isSome()) {
            return error;
          }
        }
      }

      // Normalize resources in LAUNCH.
      foreach (
          TaskInfo& task, *operation->mutable_launch()->mutable_task_infos()) {
        convertResourceFormat(
            task.mutable_resources(),
            POST_RESERVATION_REFINEMENT);

        if (task.has_executor()) {
          convertResourceFormat(
              task.mutable_executor()->mutable_resources(),
              POST_RESERVATION_REFINEMENT);
        }
      }

      return None();
    }
    case Offer::Operation::LAUNCH_GROUP: {
      // TODO(mpark): Once we perform a sanity check validation for
      // offer operations as specified in MESOS-7760, this should no
      // longer have to be handled in this function.
      if (!operation->has_launch_group()) {
        return Error(
            "A LAUNCH_GROUP offer operation must have"
            " the Offer.Operation.launch_group field set.");
      }

      Offer::Operation::LaunchGroup* launchGroup =
        operation->mutable_launch_group();

      // Validate resources in LAUNCH_GROUP.
      if (launchGroup->has_executor()) {
        Option<Error> error =
          Resources::validate(launchGroup->executor().resources());

        if (error.isSome()) {
          return error;
        }
      }

      foreach (const TaskInfo& task, launchGroup->task_group().tasks()) {
        Option<Error> error = Resources::validate(task.resources());
        if (error.isSome()) {
          return error;
        }

        if (task.has_executor()) {
          Option<Error> error =
            Resources::validate(task.executor().resources());

          if (error.isSome()) {
            return error;
          }
        }
      }

      // Normalize resources in LAUNCH_GROUP.
      if (launchGroup->has_executor()) {
        convertResourceFormat(
            launchGroup->mutable_executor()->mutable_resources(),
            POST_RESERVATION_REFINEMENT);
      }

      foreach (
          TaskInfo& task, *launchGroup->mutable_task_group()->mutable_tasks()) {
        convertResourceFormat(
            task.mutable_resources(),
            POST_RESERVATION_REFINEMENT);

        if (task.has_executor()) {
          convertResourceFormat(
              task.mutable_executor()->mutable_resources(),
              POST_RESERVATION_REFINEMENT);
        }
      }

      return None();
    }
    case Offer::Operation::CREATE_VOLUME: {
      // TODO(mpark): Once we perform a sanity check validation for
      // offer operations as specified in MESOS-7760, this should no
      // longer have to be handled in this function.
      if (!operation->has_create_volume()) {
        return Error(
            "A CREATE_VOLUME offer operation must have"
            " the Offer.Operation.create_volume field set.");
      }

      Option<Error> error =
        Resources::validate(operation->create_volume().source());

      if (error.isSome()) {
        return error;
      }

      convertResourceFormat(
          operation->mutable_create_volume()->mutable_source(),
          POST_RESERVATION_REFINEMENT);

      return None();
    }
    case Offer::Operation::DESTROY_VOLUME: {
      // TODO(mpark): Once we perform a sanity check validation for
      // offer operations as specified in MESOS-7760, this should no
      // longer have to be handled in this function.
      if (!operation->has_destroy_volume()) {
        return Error(
            "A DESTROY_VOLUME offer operation must have"
            " the Offer.Operation.destroy_volume field set.");
      }

      Option<Error> error =
        Resources::validate(operation->destroy_volume().volume());

      if (error.isSome()) {
        return error;
      }

      convertResourceFormat(
          operation->mutable_destroy_volume()->mutable_volume(),
          POST_RESERVATION_REFINEMENT);

      return None();
    }
    case Offer::Operation::CREATE_BLOCK: {
      // TODO(mpark): Once we perform a sanity check validation for
      // offer operations as specified in MESOS-7760, this should no
      // longer have to be handled in this function.
      if (!operation->has_create_block()) {
        return Error(
            "A CREATE_BLOCK offer operation must have"
            " the Offer.Operation.create_block field set.");
      }

      Option<Error> error =
        Resources::validate(operation->create_block().source());

      if (error.isSome()) {
        return error;
      }

      convertResourceFormat(
          operation->mutable_create_block()->mutable_source(),
          POST_RESERVATION_REFINEMENT);

      return None();
    }
    case Offer::Operation::DESTROY_BLOCK: {
      // TODO(mpark): Once we perform a sanity check validation for
      // offer operations as specified in MESOS-7760, this should no
      // longer have to be handled in this function.
      if (!operation->has_destroy_block()) {
        return Error(
            "A DESTROY_BLOCK offer operation must have"
            " the Offer.Operation.destroy_block field set.");
      }

      Option<Error> error =
        Resources::validate(operation->destroy_block().block());

      if (error.isSome()) {
        return error;
      }

      convertResourceFormat(
          operation->mutable_destroy_block()->mutable_block(),
          POST_RESERVATION_REFINEMENT);

      return None();
    }
    case Offer::Operation::UNKNOWN: {
      // TODO(mpark): Once we perform a sanity check validation for
      // offer operations as specified in MESOS-7760, this should no
      // longer have to be handled in this function.
      return Error("Unknown offer operation");
    }
  }
  UNREACHABLE();
}


Try<Nothing> downgradeResources(RepeatedPtrField<Resource>* resources)
{
  foreach (const Resource& resource, *resources) {
    CHECK(!resource.has_role());
    CHECK(!resource.has_reservation());
  }

  foreach (const Resource& resource, *resources) {
    if (Resources::hasRefinedReservations(resource)) {
      return Error(
          "Invalid resources downgrade: resource " + stringify(resource) +
          " with refined reservations cannot be downgraded");
    }
  }

  convertResourceFormat(resources, PRE_RESERVATION_REFINEMENT);

  return Nothing();
}

} // namespace mesos {
