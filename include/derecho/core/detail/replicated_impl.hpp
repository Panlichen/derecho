#pragma once

#include <functional>
#include <mutex>
#include <utility>

#include "../replicated.hpp"

#include <derecho/mutils-serialization/SerializationSupport.hpp>

namespace derecho {

template <typename T>
Replicated<T>::Replicated(subgroup_type_id_t type_id, node_id_t nid, subgroup_id_t subgroup_id,
                          uint32_t subgroup_index, uint32_t shard_num,
                          rpc::RPCManager& group_rpc_manager, Factory<T> client_object_factory,
                          _Group* group)
        : persistent_registry_ptr(std::make_unique<persistent::PersistentRegistry>(
                  this, std::type_index(typeid(T)), subgroup_index, shard_num)),
          user_object_ptr(std::make_unique<std::unique_ptr<T>>(
                  client_object_factory(persistent_registry_ptr.get(),subgroup_id))),
          node_id(nid),
          subgroup_id(subgroup_id),
          subgroup_index(subgroup_index),
          shard_num(shard_num),
          group_rpc_manager(group_rpc_manager),
          wrapped_this(group_rpc_manager.make_remote_invocable_class(user_object_ptr.get(),
                                                                     type_id, subgroup_id,
                                                                     T::register_functions())),
          group(group),
          subgroup_caller(type_id, nid, subgroup_id, shard_num, group_rpc_manager) {
    if constexpr(std::is_base_of_v<GroupReference, T>) {
        (**user_object_ptr).set_group_pointers(group, subgroup_index);
    }
}

template <typename T>
Replicated<T>::Replicated(subgroup_type_id_t type_id, node_id_t nid, subgroup_id_t subgroup_id,
                          uint32_t subgroup_index, uint32_t shard_num,
                          rpc::RPCManager& group_rpc_manager, _Group* group)
        : persistent_registry_ptr(std::make_unique<persistent::PersistentRegistry>(
                  this, std::type_index(typeid(T)), subgroup_index, shard_num)),
          user_object_ptr(std::make_unique<std::unique_ptr<T>>(nullptr)),
          node_id(nid),
          subgroup_id(subgroup_id),
          subgroup_index(subgroup_index),
          shard_num(shard_num),
          group_rpc_manager(group_rpc_manager),
          wrapped_this(group_rpc_manager.make_remote_invocable_class(user_object_ptr.get(),
                                                                     type_id, subgroup_id,
                                                                     T::register_functions())),
          group(group),
          subgroup_caller(nid, subgroup_id, shard_num, group_rpc_manager) {}

template <typename T>
Replicated<T>::Replicated(Replicated&& rhs) : persistent_registry_ptr(std::move(rhs.persistent_registry_ptr)),
                                              user_object_ptr(std::move(rhs.user_object_ptr)),
                                              node_id(rhs.node_id),
                                              subgroup_id(rhs.subgroup_id),
                                              subgroup_index(rhs.subgroup_index),
                                              shard_num(rhs.shard_num),
                                              group_rpc_manager(rhs.group_rpc_manager),
                                              wrapped_this(std::move(rhs.wrapped_this)),
                                              group(rhs.group),
                                              subgroup_caller(std::move(rhs.subgroup_caller)) {
    persistent_registry_ptr->updateTemporalFrontierProvider(this);
}

template <typename T>
Replicated<T>::~Replicated() {
    // hack to check if the object was merely moved
    if(wrapped_this) {
        group_rpc_manager.destroy_remote_invocable_class(subgroup_id);
    }
}

template <typename T>
void Replicated<T>::send(unsigned long long int payload_size,
                         const std::function<void(char* buf)>& msg_generator) {
    group_rpc_manager.view_manager.send(subgroup_id, payload_size, msg_generator);
}

template <typename T>
std::size_t Replicated<T>::object_size() const {
    return mutils::bytes_size(**user_object_ptr);
}

template <typename T>
void Replicated<T>::send_object(tcp::socket& receiver_socket) const {
    auto bind_socket_write = [&receiver_socket](const char* bytes, std::size_t size) {
        receiver_socket.write(bytes, size);
    };
    mutils::post_object(bind_socket_write, object_size());
    send_object_raw(receiver_socket);
}

template <typename T>
void Replicated<T>::send_object_raw(tcp::socket& receiver_socket) const {
    auto bind_socket_write = [&receiver_socket](const char* bytes, std::size_t size) {
        receiver_socket.write(bytes, size);
    };
    mutils::post_object(bind_socket_write, **user_object_ptr);
}

template <typename T>
std::size_t Replicated<T>::receive_object(char* buffer) {
    // *user_object_ptr = std::move(mutils::from_bytes<T>(&group_rpc_manager.dsm, buffer));
    mutils::RemoteDeserialization_v rdv{group_rpc_manager.rdv};
    rdv.insert(rdv.begin(), persistent_registry_ptr.get());
    mutils::DeserializationManager dsm{rdv};
    *user_object_ptr = std::move(mutils::from_bytes<T>(&dsm, buffer));
    if constexpr(std::is_base_of_v<GroupReference, T>) {
        (**user_object_ptr).set_group_pointers(group, subgroup_index);
    }
    return mutils::bytes_size(**user_object_ptr);
}

template <typename T>
void Replicated<T>::persist(const persistent::version_t version) {
    persistent::version_t persisted_ver;

    // persist variables
    do {
        persisted_ver = persistent_registry_ptr->persist();
        if(persisted_ver == -1) {
            // for replicated<T> without Persistent fields,
            // tell the persistent thread that we are done.
            persisted_ver = version;
        }
    } while(persisted_ver < version);
};

template <typename T>
const persistent::version_t Replicated<T>::get_minimum_latest_persisted_version() {
    return persistent_registry_ptr->getMinimumLatestPersistedVersion();
}

template <typename T>
const uint64_t Replicated<T>::compute_global_stability_frontier() {
    return group_rpc_manager.view_manager.compute_global_stability_frontier(subgroup_id);
}

template <typename T>
template <rpc::FunctionTag tag, typename... Args>
auto ShardIterator<T>::ordered_send(Args&&... args) {
    // shard_reps should have at least one member
    auto send_result = SC.template ordered_send<tag>(shard_reps.at(0), std::forward<Args>(args)...);
    std::vector<decltype(send_result)> send_result_vec;
    send_result_vec.emplace_back(std::move(send_result));
    for(uint i = 1; i < shard_reps.size(); ++i) {
        send_result_vec.emplace_back(SC.template ordered_send<tag>(shard_reps[i], std::forward<Args>(args)...));
    }
    return send_result_vec;
}

}  // namespace derecho
