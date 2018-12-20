/**
 * (C) 2018 KIEP Energomera, Stavropol, Apanasenkovskaya st. 4, Russia
 *
 * @file IConnection.h
 *
 * Created on: Dec 20, 2018
 * Project: sdbus-c++
 * Description: High-level D-Bus IPC C++ library based on sd-bus
 *
 * This file is part of sdbus-c++.
 *
 * sdbus-c++ is free software; you can redistribute it and/or modify it
 * under the terms of the GNU Lesser General Public License as published by
 * the Free Software Foundation, either version 2.1 of the License, or
 * (at your option) any later version.
 *
 * sdbus-c++ is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
 * GNU Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public License
 * along with sdbus-c++. If not, see <http://www.gnu.org/licenses/>.
 */

#include "EventLoopConnection.h"
#include <sdbus-c++/Message.h>
#include <sdbus-c++/Error.h>
#include "ScopeGuard.h"
#include <systemd/sd-bus.h>
#include <unistd.h>
#include <poll.h>
#include <sys/eventfd.h>
#include <sstream>

namespace sdbus { namespace internal {

void SdBusConnectionDeleter::operator()(sd_bus *ptr) {
    sd_bus_flush_close_unref(ptr);
}

EventLoopConnection::EventLoopConnection(EventLoopConnection::BusType type)
    : busType_(type)
{
    auto bus = openBus(busType_);
    bus_.reset(bus);

    finishHandshake(bus);
}

EventLoopConnection::~EventLoopConnection() {}

void EventLoopConnection::requestName(const std::string& name)
{
    auto r = sd_bus_request_name(bus_.get(), name.c_str(), 0);
    SDBUS_THROW_ERROR_IF(r < 0, "Failed to request bus name", -r);
}

void EventLoopConnection::releaseName(const std::string& name)
{
    auto r = sd_bus_release_name(bus_.get(), name.c_str());
    SDBUS_THROW_ERROR_IF(r < 0, "Failed to release bus name", -r);
}

ConnectionQueryAction EventLoopConnection::iterate() {
    if (processPendingRequest())
        return ConnectionQueryAction::HasWorkToDo;

    if (!asyncReplies_.empty()) {
        auto reply = asyncReplies_.front();
        asyncReplies_.pop();
        reply.send();
        return ConnectionQueryAction::HasWorkToDo;
    }

    return ConnectionQueryAction::WaitForEvent;
}

ConnectionPollRequest EventLoopConnection::requestPoll() {
    ConnectionPollRequest request;

    auto bus = bus_.get();

    auto r = sd_bus_get_fd(bus);
    SDBUS_THROW_ERROR_IF(r < 0, "Failed to get bus descriptor", -r);
    request.fd = r;

    r = sd_bus_get_events(bus);
    SDBUS_THROW_ERROR_IF(r < 0, "Failed to get bus events", -r);
    request.events = static_cast<short int>(r);

    uint64_t usec;
    sd_bus_get_timeout(bus, &usec);

    if (usec == uint64_t(-1))
        request.timeout = -1;
    else
        request.timeout = static_cast<int>((usec + 999) / 1000);

    return request;
}

void EventLoopConnection::enterProcessingLoopAsync()
{
}

void EventLoopConnection::leaveProcessingLoop() {}

void* EventLoopConnection::addObjectVTable( const std::string& objectPath
                                 , const std::string& interfaceName
                                 , const void* vtable
                                 , void* userData )
{
    sd_bus_slot *slot{};

    auto r = sd_bus_add_object_vtable( bus_.get()
                                     , &slot
                                     , objectPath.c_str()
                                     , interfaceName.c_str()
                                     , static_cast<const sd_bus_vtable*>(vtable)
                                     , userData );

    SDBUS_THROW_ERROR_IF(r < 0, "Failed to register object vtable", -r);

    return slot;
}

void EventLoopConnection::removeObjectVTable(void* vtableHandle)
{
    sd_bus_slot_unref((sd_bus_slot *)vtableHandle);
}

sdbus::MethodCall EventLoopConnection::createMethodCall(
                                        const std::string& destination
                                      , const std::string& objectPath
                                      , const std::string& interfaceName
                                      , const std::string& methodName ) const
{
    sd_bus_message *sdbusMsg{};

    // Returned message will become an owner of sdbusMsg
    SCOPE_EXIT{ sd_bus_message_unref(sdbusMsg); };

    auto r = sd_bus_message_new_method_call( bus_.get()
                                           , &sdbusMsg
                                           , destination.c_str()
                                           , objectPath.c_str()
                                           , interfaceName.c_str()
                                           , methodName.c_str() );

    SDBUS_THROW_ERROR_IF(r < 0, "Failed to create method call", -r);

    return MethodCall(sdbusMsg);
}

sdbus::Signal EventLoopConnection::createSignal( const std::string& objectPath
                                               , const std::string& interfaceName
                                               , const std::string& signalName ) const
{
    sd_bus_message *sdbusSignal{};

    // Returned message will become an owner of sdbusSignal
    SCOPE_EXIT{ sd_bus_message_unref(sdbusSignal); };

    auto r = sd_bus_message_new_signal( bus_.get()
                                      , &sdbusSignal
                                      , objectPath.c_str()
                                      , interfaceName.c_str()
                                      , signalName.c_str() );

    SDBUS_THROW_ERROR_IF(r < 0, "Failed to create signal", -r);

    return Signal(sdbusSignal);
}

void* EventLoopConnection::registerSignalHandler( const std::string& objectPath
                                                , const std::string& interfaceName
                                                , const std::string& signalName
                                                , sd_bus_message_handler_t callback
                                                , void* userData )
{
    sd_bus_slot *slot{};

    auto filter = composeSignalMatchFilter(objectPath, interfaceName, signalName);
    auto r = sd_bus_add_match(bus_.get(), &slot, filter.c_str(), callback, userData);

    SDBUS_THROW_ERROR_IF(r < 0, "Failed to register signal handler", -r);

    return slot;
}

void EventLoopConnection::unregisterSignalHandler(void* handlerCookie)
{
    sd_bus_slot_unref((sd_bus_slot *)handlerCookie);
}

void EventLoopConnection::sendReplyAsynchronously(const sdbus::MethodReply& reply)
{
    asyncReplies_.push(reply);
}

std::unique_ptr<sdbus::internal::IConnection> EventLoopConnection::clone() const
{
    return std::make_unique<sdbus::internal::EventLoopConnection>(busType_);
}

sd_bus* EventLoopConnection::openBus(EventLoopConnection::BusType type)
{
    sd_bus* bus{};

    int r = -1;

    switch (type) {
        case BusType::eSession:
            r = sd_bus_open_user(&bus);
        break;
        case BusType::eSystem:
            r = sd_bus_open_system(&bus);
        break;
    }

    SDBUS_THROW_ERROR_IF(r < 0, "Failed to open bus", -r);
    assert(bus != nullptr);

    return bus;
}

void EventLoopConnection::finishHandshake(sd_bus* bus)
{
    // Process all requests that are part of the initial handshake,
    // like processing the Hello message response, authentication etc.,
    // to avoid connection authentication timeout in dbus daemon.

    assert(bus != nullptr);

    auto r = sd_bus_flush(bus);

    SDBUS_THROW_ERROR_IF(r < 0, "Failed to flush bus on opening", -r);
}

bool EventLoopConnection::processPendingRequest()
{
    auto bus = bus_.get();

    assert(bus != nullptr);

    int r = sd_bus_process(bus, nullptr);

    SDBUS_THROW_ERROR_IF(r < 0, "Failed to process bus requests", -r);

    return r > 0;
}

std::string EventLoopConnection::composeSignalMatchFilter( const std::string& objectPath
                                                         , const std::string& interfaceName
                                                         , const std::string& signalName )
{
    std::ostringstream builder;

    builder << "type='signal',"
            << "interface='" << interfaceName << "',"
            << "member='"    << signalName    << "',"
            << "path='"      << objectPath    << "'";

    return builder.str();
}

}}

namespace sdbus {

std::unique_ptr<sdbus::IEventConnection> createSystemEventConnection (const std::string &name)
{
    auto bus = std::make_unique<sdbus::internal::EventLoopConnection>(internal::EventLoopConnection::BusType::eSystem);

    if (!name.empty())
        bus->requestName(name);

    return bus;
}


std::unique_ptr<sdbus::IEventConnection> createSessionEventConnection(const std::string &name)
{
    auto bus = std::make_unique<sdbus::internal::EventLoopConnection>(internal::EventLoopConnection::BusType::eSession);

    if (!name.empty())
        bus->requestName(name);

    return bus;
}

}
