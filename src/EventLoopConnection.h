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

#ifndef EVENTLOOPCONNECTION_H
#define EVENTLOOPCONNECTION_H

#include <sdbus-c++/IConnection.h>
#include <sdbus-c++/Message.h>
#include "IConnection.h"
#include <systemd/sd-bus.h>
#include <memory>
#include <thread>
#include <atomic>
#include <mutex>
#include <queue>

namespace sdbus { namespace internal {

    struct SdBusConnectionDeleter {
        void operator() (sd_bus *ptr);
    };

    class EventLoopConnection
        : public sdbus::IEventConnection      // External, public interface
        , public sdbus::internal::IConnection // Internal, private interface
    {
        using SDBusPtr = std::unique_ptr<sd_bus, SdBusConnectionDeleter>;

    public:
        enum class BusType
        {
            eSystem,
            eSession
        };

        EventLoopConnection(BusType type);
        ~EventLoopConnection() override;

        void requestName(const std::string& name) override;
        void releaseName(const std::string& name) override;

        ConnectionQueryAction iterate() override;
        ConnectionPollRequest requestPoll() override;

        void enterProcessingLoopAsync() override;
        void leaveProcessingLoop() override;

        void* addObjectVTable( const std::string& objectPath
                             , const std::string& interfaceName
                             , const void* vtable
                             , void* userData ) override;
        void removeObjectVTable(void* vtableHandle) override;

        sdbus::MethodCall createMethodCall( const std::string& destination
                                          , const std::string& objectPath
                                          , const std::string& interfaceName
                                          , const std::string& methodName ) const override;
        sdbus::Signal createSignal( const std::string& objectPath
                                  , const std::string& interfaceName
                                  , const std::string& signalName ) const override;

        void* registerSignalHandler( const std::string& objectPath
                                   , const std::string& interfaceName
                                   , const std::string& signalName
                                   , sd_bus_message_handler_t callback
                                   , void* userData ) override;
        void unregisterSignalHandler(void* handlerCookie) override;

        void sendReplyAsynchronously(const sdbus::MethodReply& reply) override;

        std::unique_ptr<sdbus::internal::IConnection> clone() const override;

    private:
        static sd_bus* openBus(EventLoopConnection::BusType type);
        static void finishHandshake(sd_bus* bus);
        bool processPendingRequest();
        static std::string composeSignalMatchFilter( const std::string& objectPath
                                                   , const std::string& interfaceName
                                                   , const std::string& signalName );

    private:
        SDBusPtr bus_;
        std::queue<MethodReply> asyncReplies_;
        BusType busType_;

        static constexpr const uint64_t POLL_TIMEOUT_USEC = 500000;
    };
}}

#endif // EVENTLOOPCONNECTION_H
