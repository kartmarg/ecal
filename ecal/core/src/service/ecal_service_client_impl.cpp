/* ========================= eCAL LICENSE =================================
 *
 * Copyright (C) 2016 - 2019 Continental Corporation
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 * 
 *      http://www.apache.org/licenses/LICENSE-2.0
 * 
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 *
 * ========================= eCAL LICENSE =================================
*/

/**
 * @brief  eCAL service client implementation
**/

#include "ecal_global_accessors.h"

#include "ecal_registration_provider.h"
#include "ecal_clientgate.h"
#include "ecal_service_client_impl.h"

#include <chrono>
#include <sstream>
#include <utility>

#include "ecal_service_singleton_manager.h"

namespace eCAL
{
  /**
   * @brief Service client implementation class.
  **/
  CServiceClientImpl::CServiceClientImpl() :
    m_response_callback(nullptr),
    m_created(false)
  {
  }

  CServiceClientImpl::CServiceClientImpl(const std::string& service_name_) :
    m_response_callback(nullptr),
    m_created(false)
  {
    Create(service_name_);
  }

  CServiceClientImpl::~CServiceClientImpl()
  {
    Destroy();
  }

  bool CServiceClientImpl::Create(const std::string& service_name_)
  {
    if (m_created) return(false);

    // set service name
    m_service_name = service_name_;

    // create service id
    std::stringstream counter;
    counter << std::chrono::steady_clock::now().time_since_epoch().count();
    m_service_id = counter.str();

    // register this client
    Register(false);

    // mark as created
    m_created = true;

    return(true);
  }

  bool CServiceClientImpl::Destroy()
  {
    if (!m_created) return(false);

    // reset client map
    {
      std::lock_guard<std::mutex> const lock(m_client_map_sync);
      m_client_map.clear();
    }

    // reset method callback map
    {
      std::lock_guard<std::mutex> const lock(m_response_callback_sync);
      m_response_callback = nullptr;
    }

    // reset event callback map
    {
      std::lock_guard<std::mutex> const lock(m_event_callback_map_sync);
      m_event_callback_map.clear();
    }

    // unregister this client
    Unregister();

    // reset internals
    m_service_name.clear();
    m_service_id.clear();
    m_host_name.clear();

    // mark as not created
    m_created = false;

    return(true);
  }

  bool CServiceClientImpl::SetHostName(const std::string& host_name_)
  {
    if (host_name_ == "*") m_host_name.clear();
    else                   m_host_name = host_name_;
    return(true);
  }

  // add callback function for service response
  bool CServiceClientImpl::AddResponseCallback(const ResponseCallbackT& callback_)
  {
    std::lock_guard<std::mutex> const lock(m_response_callback_sync);
    m_response_callback = callback_;
    return true;
  }

  // remove callback function for service response
  bool CServiceClientImpl::RemResponseCallback()
  {
    std::lock_guard<std::mutex> const lock(m_response_callback_sync);
    m_response_callback = nullptr;
    return true;
  }

  // add callback function for client events
  bool CServiceClientImpl::AddEventCallback(eCAL_Client_Event type_, ClientEventCallbackT callback_)
  {
    if (!m_created) return false;

    // store event callback
    {
      std::lock_guard<std::mutex> const lock(m_event_callback_map_sync);
#ifndef NDEBUG
      // log it
      Logging::Log(log_level_debug2, m_service_name + "::CServiceClientImpl::AddEventCallback");
#endif
      m_event_callback_map[type_] = std::move(callback_);
    }

    return true;
  }

  // remove callback function for client events
  bool CServiceClientImpl::RemEventCallback(eCAL_Client_Event type_)
  {
    if (!m_created) return false;

    // reset event callback
    {
      std::lock_guard<std::mutex> const lock(m_event_callback_map_sync);
#ifndef NDEBUG
      // log it
      Logging::Log(log_level_debug2, m_service_name + "::CServiceClientImpl::RemEventCallback");
#endif
      m_event_callback_map[type_] = nullptr;
    }

    return true;
  }

  // blocking call, no broadcast, first matching service only, response will be returned in service_response_
  [[deprecated]]
  bool CServiceClientImpl::Call(const std::string& method_name_, const std::string& request_, struct SServiceResponse& service_response_)
  {
    if (g_clientgate() == nullptr) return false;
    if (!m_created)                return false;

    if (m_service_name.empty()
      || method_name_.empty()
      )
      return false;

    // check for new server
    CheckForNewServices();

    std::vector<SServiceAttr> const service_vec = g_clientgate()->GetServiceAttr(m_service_name);
    for (const auto& iter : service_vec)
    {
      if (m_host_name.empty() || (m_host_name == iter.hname))
      {
        std::lock_guard<std::mutex> const lock(m_client_map_sync);
        auto client = m_client_map.find(iter.key);
        if (client != m_client_map.end())
        {
          // Copy raw request in a protocol buffer TODO: This is kind of unnecessary and part of the protocol, so it also shouldn't be done here.
          eCAL::pb::Request request_pb;
          request_pb.mutable_header()->set_mname(method_name_);
          request_pb.set_request(request_);
          auto request_shared_ptr  = std::make_shared<std::string>();
          auto response_shared_ptr = std::make_shared<std::string>();
          *request_shared_ptr      = request_pb.SerializeAsString();
          
          auto error = client->second->call_service(request_shared_ptr, response_shared_ptr);
          if (!error)
          {
            fromSerializedProtobuf(*response_shared_ptr, service_response_);
            return true;
          }
        }
      }
    }
    return false;
  }

  // blocking call, all responses will be returned in service_response_vec_
  bool CServiceClientImpl::Call(const std::string& method_name_, const std::string& request_, int timeout_, ServiceResponseVecT* service_response_vec_)
  {
    if (g_clientgate() == nullptr) return false;
    if (!m_created)                return false;

    if (m_service_name.empty()
      || method_name_.empty()
      )
      return false;

    // reset response
    if(service_response_vec_ != nullptr) service_response_vec_->clear();

    // check for new server
    CheckForNewServices();

    // Copy raw request in a protocol buffer TODO: This is kind of unnecessary and part of the protocol, so it also shouldn't be done here.
    eCAL::pb::Request request_pb;
    request_pb.mutable_header()->set_mname(method_name_);
    request_pb.set_request(request_);
    auto request_shared_ptr = std::make_shared<std::string>();
    *request_shared_ptr = request_pb.SerializeAsString();

    std::vector<SServiceAttr> const service_vec = g_clientgate()->GetServiceAttr(m_service_name);

    // Create a condition variable and a mutex to wait for the response
    // All variables are in shared pointers, as we need to pass them to the
    // callback function via the lambda capture. When the user uses the timeout,
    // this method may finish earlier than the service calls, so we need to make
    // sure the callbacks can still operate on those variables.
    const auto  mutex                       = std::make_shared<std::mutex>();
    const auto  condition_variable          = std::make_shared<std::condition_variable>();
    const auto  responses                   = std::make_shared<ServiceResponseVecT>();
    const auto  finished_service_call_count = std::make_shared<int>(0);

    const auto  expected_service_call_count = std::make_shared<int>(0);

    // Iterate over all service sessions and call each of them.
    // Each successfull call will increment the finished_service_call_count.
    // By comparing that with the expected_service_call_count we later know
    // Whether all service calls have been completed.
    for (const auto& service : service_vec)
    {
      // Only call service if host name matches
      if (m_host_name.empty() || (m_host_name == service.hname))
      {
        // Lock mutex for iterating over client session map
        std::lock_guard<std::mutex> const client_map_lock(m_client_map_sync);

        // Find the actual client session in the map
        auto client = m_client_map.find(service.key);
        if (client != m_client_map.end())
        {
          eCAL::service::ClientResponseCallbackT response_callback;

          {
            const std::lock_guard<std::mutex> lock(*mutex);
            (*expected_service_call_count)++;
            responses->emplace_back();
            responses->back().host_name    = service.hname;
            responses->back().service_name = service.sname;
            responses->back().service_id   = service.key;
            responses->back().method_name  = method_name_;
            responses->back().error_msg    = "Timeout";
            responses->back().ret_state    = 0;
            responses->back().call_state   = eCallState::call_state_failed;
            responses->back().response     = "";
            // TODO: Looks like I need to also call some event callback here, for every call that timeouted

            // Create a response callback, that will set the response and notify the condition variable
            response_callback
                      = [mutex, condition_variable, responses, finished_service_call_count, i = (responses->size() - 1)]
                        (const eCAL::service::Error& response_error, const std::shared_ptr<std::string>& response_)
                        {
                          const std::lock_guard<std::mutex> lock(*mutex);

                          if (response_error)
                          {
                            (*responses)[i].error_msg    = response_error.ToString();
                            (*responses)[i].call_state   = eCallState::call_state_failed;
                            (*responses)[i].ret_state    = 0;
                          }
                          else
                          {
                            fromSerializedProtobuf(*response_, (*responses)[i]);
                          }

                          (*finished_service_call_count)++;
                          condition_variable->notify_all();
                        };

          } // unlock mutex

          // Call service asynchronously
          client->second->async_call_service(request_shared_ptr, response_callback);
        }

      }
    }

    // Lock mutex, call service asynchronously and wait for the condition variable to be notified
    {
      std::unique_lock<std::mutex> lock(*mutex);
      if (timeout_ > 0)
      {
        condition_variable->wait_for(lock
                                    , std::chrono::milliseconds(timeout_)
                                    , [&expected_service_call_count, &finished_service_call_count]()
                                      {
                                        // Wait for all services to return something
                                        return *expected_service_call_count == *finished_service_call_count;
                                      });
      }
      else
      {
        condition_variable->wait(lock, [&expected_service_call_count, &finished_service_call_count]()
                                        {
                                          // Wait for all services to return something
                                          return *expected_service_call_count == *finished_service_call_count;
                                        });
      }

      // Now just copy our temporary return vector to the user's return vector
      if (service_response_vec_ != nullptr)
      {
        service_response_vec_->clear();
        service_response_vec_->resize(responses->size());
        for (int i = 0; i < responses->size(); i++)
        {
          (*service_response_vec_)[i] = (*responses)[i];
        }
      }

      // Determine if any call has been successful
      for (int i = 0; i < responses->size(); i++)
      {
        if ((*responses)[i].call_state == call_state_executed)
          return true;
      }
      return false;
    }


    //=== Old code, TODO: remove! ====

    //bool called(false);
    //std::vector<SServiceAttr> const service_vec = g_clientgate()->GetServiceAttr(m_service_name);
    //for (const auto& iter : service_vec)
    //{
    //  if (m_host_name.empty() || (m_host_name == iter.hname))
    //  {
    //    std::lock_guard<std::mutex> const lock(m_client_map_sync);
    //    auto client = m_client_map.find(iter.key);
    //    if (client != m_client_map.end())
    //    {
    //      struct SServiceResponse service_response;
    //      if (SendRequest(client->second, method_name_, request_, timeout_, service_response))
    //      {
    //        if(service_response_vec_ != nullptr) service_response_vec_->push_back(service_response);
    //        called = true;
    //      }
    //    }
    //  }
    //}
    //return called;
  }

  // blocking call, using callback
  bool CServiceClientImpl::Call(const std::string& method_name_, const std::string& request_, int timeout_)
  {
    // Create response vector
    ServiceResponseVecT response_vec;

    // Call all services blocking
    bool success = Call(method_name_, request_, timeout_, &response_vec);

    // iterate over responses and call the callbacks
    for (const auto& response : response_vec)
    {
      std::lock_guard<std::mutex> const lock_cb(m_response_callback_sync);
      if (m_response_callback) m_response_callback(response);
    }

    return success;

    // ========== Old Code, TODO: Remove ==========

    //if (g_clientgate() == nullptr) return false;
    //if (!m_created)                return false;

    //if (m_service_name.empty()
    //  || method_name_.empty()
    //  )
    //  return false;

    //// check for new server
    //CheckForNewServices();

    //// send request to every single service
    //return SendRequests(m_host_name, method_name_, request_, timeout_);
  }

  // asynchronously call, using callback
  bool CServiceClientImpl::CallAsync(const std::string& method_name_, const std::string& request_ /*, int timeout_*/)
  {
    // TODO: implement timeout

    if (g_clientgate() == nullptr)
    {
      ErrorCallback(method_name_, "Clientgate error.");
      return false;
    }

    if (!m_created)
    {
      ErrorCallback(method_name_, "Client hasn't been created yet.");
      return false;
    }

    if (m_service_name.empty()
      || method_name_.empty())
    {
      ErrorCallback(method_name_, "Invalid service or method name.");
      return false;
    }

    // check for new server
    CheckForNewServices();

    // Copy raw request in a protocol buffer TODO: This is kind of unnecessary and part of the protocol, so it also shouldn't be done here.
    eCAL::pb::Request request_pb;
    request_pb.mutable_header()->set_mname(method_name_);
    request_pb.set_request(request_);
    auto request_shared_ptr = std::make_shared<std::string>();
    *request_shared_ptr = request_pb.SerializeAsString();

    bool at_least_one_service_was_called (false);

    // Call all services
    std::vector<SServiceAttr> const service_vec = g_clientgate()->GetServiceAttr(m_service_name);
    for (const auto& service : service_vec)
    {
      if (m_host_name.empty() || (m_host_name == service.hname))
      {
        std::lock_guard<std::mutex> const lock(m_client_map_sync);
        auto client = m_client_map.find(service.key);
        if (client != m_client_map.end())
        {
          const eCAL::service::ClientResponseCallbackT response_callback
                      = [hostname = service.hname, servicename = service.sname, this] // TODO: using the this pointer here actually firces us to also manage the lifetime of this, e.g. with a shared ptr
                        (const eCAL::service::Error& response_error, const std::shared_ptr<std::string>& response_)
                        {
                          std::lock_guard<std::mutex> const lock(this->m_response_callback_sync);
                          
                          if (m_response_callback)
                          {
                            eCAL::SServiceResponse service_response_struct;
                            
                            service_response_struct.host_name    = hostname;
                            service_response_struct.service_name = servicename;

                            if (response_error)
                            {
                              service_response_struct.error_msg    = response_error.ToString();
                              service_response_struct.call_state   = eCallState::call_state_failed;
                              service_response_struct.ret_state    = 0;
                            }
                            else
                            {
                              fromSerializedProtobuf(*response_, service_response_struct);
                            }

                            this->m_response_callback(service_response_struct);
                          }
                        };

          client->second->async_call_service(request_shared_ptr, response_callback);

          at_least_one_service_was_called = true;
        }
      }
    }

    return(at_least_one_service_was_called);

    //================ Old Code. TODO: remove ==============

    //bool called(false);
    //std::vector<SServiceAttr> const service_vec = g_clientgate()->GetServiceAttr(m_service_name);
    //for (const auto& iter : service_vec)
    //{
    //  if (m_host_name.empty() || (m_host_name == iter.hname))
    //  {
    //    std::lock_guard<std::mutex> const lock(m_client_map_sync);
    //    auto client = m_client_map.find(iter.key);
    //    if (client != m_client_map.end())
    //    {
    //      SendRequestAsync(client->second, method_name_, request_ /*, timeout_*/, -1);
    //      called = true;
    //    }
    //  }
    //}
    //return(called);
  }

  // check connection state
  bool CServiceClientImpl::IsConnected()
  {
    if (!m_created) return false;

    // check for connected clients
    std::lock_guard<std::mutex> const lock(m_connected_services_map_sync);
    return !m_connected_services_map.empty();
  }

  // called by the eCAL::CClientGate to register a service
  void CServiceClientImpl::RegisterService(const std::string& key_, const SServiceAttr& service_)
  {
    // check connections
    std::lock_guard<std::mutex> const lock(m_connected_services_map_sync);

    // is this a new connection ?
    if (m_connected_services_map.find(key_) == m_connected_services_map.end())
    {
      // call connect event
      std::lock_guard<std::mutex> const lock_eb(m_event_callback_map_sync);
      auto e_iter = m_event_callback_map.find(client_event_connected);
      if (e_iter != m_event_callback_map.end())
      {
        SClientEventCallbackData sdata;
        sdata.type = client_event_connected;
        sdata.time = std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
        sdata.attr = service_;
        (e_iter->second)(m_service_name.c_str(), &sdata);
      }
      // add service
      m_connected_services_map[key_] = service_;
    }
  }

  // called by eCAL:CClientGate every second to update registration layer
  void CServiceClientImpl::RefreshRegistration()
  {
    if (!m_created) return;
    Register(false);
  }

  void CServiceClientImpl::fromSerializedProtobuf(const std::string& response_pb_string, eCAL::SServiceResponse& response)
  {
    eCAL::pb::Response response_pb;
    if (response_pb.ParseFromString(response_pb_string))
    {
      fromProtobuf(response_pb, response);
    }
    else
    {
      response.error_msg  = "Could not parse server response";
      response.ret_state  = 0;
      response.call_state = eCallState::call_state_failed;
      response.response   = "";
    }
  }

  void CServiceClientImpl::fromProtobuf(const eCAL::pb::Response& response_pb, eCAL::SServiceResponse& response)
  {
    const auto& response_pb_header = response_pb.header();
    response.host_name    = response_pb_header.hname();
    response.service_name = response_pb_header.sname();
    response.service_id   = response_pb_header.sid();
    response.method_name  = response_pb_header.mname();
    response.error_msg    = response_pb_header.error();
    response.ret_state    = static_cast<int>(response_pb.ret_state());
    switch (response_pb_header.state())
    {
    case eCAL::pb::ServiceHeader_eCallState_executed:
      response.call_state = call_state_executed;
      break;
    case eCAL::pb::ServiceHeader_eCallState_failed:
      response.call_state = call_state_failed;
      break;
    default:
      break;
    }
    response.response = response_pb.response();
  }

  void CServiceClientImpl::Register(const bool force_)
  {
    if (m_service_name.empty()) return;

    eCAL::pb::Sample sample;
    sample.set_cmd_type(eCAL::pb::bct_reg_client);
    auto *service_mutable_client = sample.mutable_client();
    service_mutable_client->set_version(m_client_version);
    service_mutable_client->set_hname(Process::GetHostName());
    service_mutable_client->set_pname(Process::GetProcessName());
    service_mutable_client->set_uname(Process::GetUnitName());
    service_mutable_client->set_pid(Process::GetProcessID());
    service_mutable_client->set_sname(m_service_name);
    service_mutable_client->set_sid(m_service_id);

    // register entity
    if (g_registration_provider() != nullptr) g_registration_provider()->RegisterClient(m_service_name, m_service_id, sample, force_);

    // refresh connected services map
    CheckForNewServices();

    // check for disconnected services
    {
      std::lock_guard<std::mutex> const lock(m_client_map_sync);
      for (auto& client : m_client_map)
      {
        if (client.second->get_state() == eCAL::service::State::FAILED)
        {
          std::string const service_key = client.first;

          // is the service still in the connecting map ?
          auto iter = m_connected_services_map.find(service_key);
          if (iter != m_connected_services_map.end())
          {
            // call disconnect event
            std::lock_guard<std::mutex> const lock_cb(m_event_callback_map_sync);
            auto e_iter = m_event_callback_map.find(client_event_disconnected);
            if (e_iter != m_event_callback_map.end())
            {
              SClientEventCallbackData sdata;
              sdata.type = client_event_disconnected;
              sdata.time = std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
              sdata.attr = iter->second;
              (e_iter->second)(m_service_name.c_str(), &sdata);
            }
            // remove service
            m_connected_services_map.erase(iter);
          }
        }
      }
    }
  }

  void CServiceClientImpl::Unregister()
  {
    if (m_service_name.empty()) return;

    eCAL::pb::Sample sample;
    sample.set_cmd_type(eCAL::pb::bct_unreg_client);
    auto* service_mutable_client = sample.mutable_client();
    service_mutable_client->set_hname(Process::GetHostName());
    service_mutable_client->set_pname(Process::GetProcessName());
    service_mutable_client->set_uname(Process::GetUnitName());
    service_mutable_client->set_pid(Process::GetProcessID());
    service_mutable_client->set_sname(m_service_name);
    service_mutable_client->set_sid(m_service_id);
    service_mutable_client->set_version(m_client_version);

    // unregister entity
    if (g_registration_provider() != nullptr) g_registration_provider()->UnregisterClient(m_service_name, m_service_id, sample, true);
  }

  void CServiceClientImpl::CheckForNewServices()
  {
    if (g_clientgate() == nullptr) return;

    // check for new services
    std::vector<SServiceAttr> const service_vec = g_clientgate()->GetServiceAttr(m_service_name);
    for (const auto& iter : service_vec)
    {
      std::lock_guard<std::mutex> const lock(m_client_map_sync);
      auto client = m_client_map.find(iter.key);
      if (client == m_client_map.end())
      {
        auto client_manager = eCAL::service::ServiceManager::instance()->get_client_manager();
        if (client_manager == nullptr || client_manager->is_stopped()) return;

        // Event callback
        // TODO: Make an actual implementation
        const eCAL::service::ClientSession::EventCallbackT event_callback
                = []
                  (eCAL_Client_Event /*event*/, const std::string& /*message*/) -> void
                  {};

        // Only connect via V0 protocol / V0 port, if V1 port is not available
        const auto protocol_version = (iter.tcp_port_v1 != 0 ? iter.version : 0);
        const auto port_to_use = (protocol_version == 0 ? iter.tcp_port_v0 : iter.tcp_port_v1);

        // Create the client and add it to the map
        const auto new_client_session = client_manager->create_client(protocol_version, iter.hname, port_to_use, event_callback);
        if (new_client_session)
          m_client_map[iter.key] = new_client_session;
      }
    }
  }

  //bool CServiceClientImpl::SendRequests(const std::string& host_name_, const std::string& method_name_, const std::string& request_, int timeout_)
  //{
  //  if (g_clientgate() == nullptr) return false;

  //  bool ret_state(false);

  //  std::lock_guard<std::mutex> const lock(m_client_map_sync);
  //  for (auto& client : m_client_map)
  //  {
  //    if (client.second->get_state() != eCAL::service::State::FAILED)
  //    {
  //      if (host_name_.empty() || (host_name_ == client.second->get_address()))
  //      {
  //        // execute request
  //        SServiceResponse service_response;
  //        ret_state = SendRequest(client.second, method_name_, request_, timeout_, service_response);
  //        if (!ret_state)
  //        {
  //          std::cerr << "CServiceClientImpl::SendRequests failed." << std::endl;
  //        }
  //        
  //        // call response callback
  //        if (service_response.call_state != call_state_none)
  //        {
  //          std::lock_guard<std::mutex> const lock_cb(m_response_callback_sync);
  //          if (m_response_callback) m_response_callback(service_response);
  //        }
  //        else
  //        {
  //          // call_state_none means service no more available
  //          // we destroy the client here
  //          client.second->stop();

  //          // TODO: Remove the client from the map!?!? I think we currently keep a non-functional client here.
  //        }
  //        // collect return state
  //        ret_state = true;
  //      }
  //    }
  //  }
  //  return ret_state;
  //}

  //bool CServiceClientImpl::SendRequest(const std::shared_ptr<eCAL::service::ClientSession>& client_, const std::string& method_name_, const std::string& request_, int timeout_, struct SServiceResponse& service_response_)
  //{
  //  // create request protocol buffer
  //  eCAL::pb::Request request_pb;
  //  request_pb.mutable_header()->set_mname(method_name_);
  //  request_pb.set_request(request_);
  //  std::string const request_s = request_pb.SerializeAsString();

  //  // catch events
  //  client_->AddEventCallback([this](eCAL_Client_Event event, const std::string& /*message*/)
  //    {
  //      switch (event)
  //      {
  //      case client_event_timeout:
  //      {
  //        std::lock_guard<std::mutex> const lock_eb(m_event_callback_map_sync);
  //        auto e_iter = m_event_callback_map.find(client_event_timeout);
  //        if (e_iter != m_event_callback_map.end())
  //        {
  //          SClientEventCallbackData sdata;
  //          sdata.type = client_event_timeout;
  //          sdata.time = std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
  //          (e_iter->second)(m_service_name.c_str(), &sdata);
  //        }
  //      }
  //        break;
  //      default:
  //        break;
  //      }
  //    });

  //  // execute request
  //  std::string response_s;
  //  size_t const sent = client_->ExecuteRequest(request_s, timeout_, response_s);
  //  if (sent == 0) return false;

  //  // parse response protocol buffer
  //  eCAL::pb::Response response_pb;
  //  if (!response_pb.ParseFromString(response_s))
  //  {
  //    std::cerr << "CServiceClientImpl::SendRequest Could not parse server response !" << std::endl;
  //    return false;
  //  }

  //  const auto& response_pb_header = response_pb.header();
  //  service_response_.host_name    = response_pb_header.hname();
  //  service_response_.service_name = response_pb_header.sname();
  //  service_response_.service_id   = response_pb_header.sid();
  //  service_response_.method_name  = response_pb_header.mname();
  //  service_response_.error_msg    = response_pb_header.error();
  //  service_response_.ret_state    = static_cast<int>(response_pb.ret_state());
  //  switch (response_pb_header.state())
  //  {
  //  case eCAL::pb::ServiceHeader_eCallState_executed:
  //    service_response_.call_state = call_state_executed;
  //    break;
  //  case eCAL::pb::ServiceHeader_eCallState_failed:
  //    service_response_.call_state = call_state_failed;
  //    break;
  //  default:
  //    break;
  //  }
  //  service_response_.response = response_pb.response();

  //  return (service_response_.call_state == call_state_executed);
  //}

  //void CServiceClientImpl::SendRequestAsync(const std::shared_ptr<eCAL::service::ClientSession>& client_, const std::string& method_name_, const std::string& request_, int timeout_)
  //{
  //  // create request protocol buffer
  //  eCAL::pb::Request request_pb;
  //  request_pb.mutable_header()->set_mname(method_name_);
  //  request_pb.set_request(request_);
  //  std::string const request_s = request_pb.SerializeAsString();

  //  client_->ExecuteRequestAsync(request_s, timeout_, [this, client_, method_name_](const std::string& response, bool success)
  //    {
  //      std::lock_guard<std::mutex> const lock(m_response_callback_sync);
  //      if (m_response_callback)
  //      {
  //        SServiceResponse service_response;
  //        if (!success)
  //        {
  //          const auto *error_msg = "CServiceClientImpl::SendRequestAsync failed !";
  //          service_response.call_state  = call_state_failed;
  //          service_response.error_msg   = error_msg;
  //          service_response.ret_state   = 0;
  //          service_response.method_name = method_name_;
  //          service_response.response.clear();
  //          m_response_callback(service_response);
  //          return;
  //        }

  //        eCAL::pb::Response response_pb;
  //        if (!response_pb.ParseFromString(response))
  //        {
  //          const auto *error_msg = "CServiceClientImpl::SendRequestAsync could not parse server response !";
  //          std::cerr << error_msg << "\n";
  //          service_response.call_state  = call_state_failed;
  //          service_response.error_msg   = error_msg;
  //          service_response.ret_state   = 0;
  //          service_response.method_name = method_name_;
  //          service_response.response.clear();
  //          m_response_callback(service_response);
  //          return;
  //        }

  //        const auto& response_pb_header = response_pb.header();
  //        service_response.host_name    = response_pb_header.hname();
  //        service_response.service_name = response_pb_header.sname();
  //        service_response.service_id   = response_pb_header.sid();
  //        service_response.method_name  = response_pb_header.mname();
  //        service_response.error_msg    = response_pb_header.error();
  //        service_response.ret_state    = static_cast<int>(response_pb.ret_state());
  //        switch (response_pb_header.state())
  //        {
  //        case eCAL::pb::ServiceHeader_eCallState_executed:
  //          service_response.call_state = call_state_executed;
  //          break;
  //        case eCAL::pb::ServiceHeader_eCallState_failed:
  //          service_response.call_state = call_state_failed;
  //          break;
  //        default:
  //          break;
  //        }
  //        service_response.response = response_pb.response();

  //        m_response_callback(service_response);
  //      }
  //    });
  //}

  void CServiceClientImpl::ErrorCallback(const std::string& method_name_, const std::string& error_message_)
  {
    std::lock_guard<std::mutex> const lock(m_response_callback_sync);
    if (m_response_callback)
    {
      SServiceResponse service_response;
      service_response.call_state  = call_state_failed;
      service_response.error_msg   = error_message_;
      service_response.ret_state   = 0;
      service_response.method_name = method_name_;
      service_response.response.clear();
      m_response_callback(service_response);
    }
  }
}
