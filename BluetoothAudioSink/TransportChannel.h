/*
 * If not stated otherwise in this file or this component's LICENSE file the
 * following copyright and licenses apply:
 *
 * Copyright 2021 Metrological
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 * http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#pragma once

#include "Module.h"
#include "IAudioCodec.h"
#include "Tracing.h"

namespace WPEFramework {

namespace A2DP {

    class TransportChannel : public Bluetooth::RTPSocket {
    private:
        static constexpr uint16_t A2DP_OMTU = 672;

        // Payload type should be a value from the dynamic range (96-127).
        // Typically 96 is chosen for A2DP implementations.
        static constexpr uint8_t A2DP_PAYLOAD_TYPE = 96;

        static constexpr uint16_t OpenTimeout = 2000; // ms
        static constexpr uint16_t CloseTimeout = 5000;
        static constexpr uint16_t PacketTimeout = 250;

    public:
        TransportChannel() = delete;
        TransportChannel(const TransportChannel&) = delete;
        TransportChannel& operator=(const TransportChannel&) = delete;

        TransportChannel(const uint8_t ssrc, const Core::NodeId& localNode, const Core::NodeId& remoteNode)
            : Bluetooth::RTPSocket(localNode, remoteNode)
            , _lock()
            , _codec(nullptr)
            , _ssrc(ssrc)
            , _timestamp(0)
            , _sequence(0)
        {
        }
        ~TransportChannel()
        {
            Disconnect();
        }

    private:
        uint32_t Initialize() override
        {
            return (Core::ERROR_NONE);
        }
        void Operational(const bool upAndRunning) override
        {
            TRACE(TransportFlow, (_T("Bluetooth A2DP/RTP transport channel is now %soperational"), (upAndRunning == true? "" : "in")));

            if (upAndRunning == true) {
                uint32_t flushable = 1;
                if (setsockopt(Handle(), SOL_BLUETOOTH, BT_FLUSHABLE, &flushable, sizeof(flushable)) < 0) {
                    TRACE(Trace::Error, (_T("Failed to set the RTP socket flushable")));
                }
            }
        }

    public:
        uint32_t Connect(const Core::NodeId& remoteNode, IAudioCodec* codec)
        {
            RemoteNode(remoteNode);

            uint32_t result = Open(OpenTimeout);
            if (result != Core::ERROR_NONE) {
                TRACE(Trace::Error, (_T("Failed to open A2DP/RTP transport socket [%d]"), result));
            } else {
                TRACE(TransportFlow, (_T("Successfully opened A2DP/RTP transport socket")));
                _codec = codec;
                Reset();
            }

            return (result);
        }
        uint32_t Disconnect()
        {
            uint32_t result = Core::ERROR_NONE;

            _codec = nullptr;

            if (IsOpen() == true) {
                result = Close(CloseTimeout);
                if (result != Core::ERROR_NONE) {
                    TRACE(Trace::Error, (_T("Failed to close AVDTP/RTP transport socket [%d]"), result));
                } else {
                    TRACE(TransportFlow, (_T("Successfully closed AVDTP/RTP transport socket")));
                }
            }

            return (result);
        }

    public:
        uint32_t Timestamp() const
        {
            return (_timestamp);
        }
        uint32_t ClockRate() const
        {
            ASSERT(_codec != nullptr);
            return (_codec->ClockRate());
        }
        uint8_t Channels() const
        {
            ASSERT(_codec != nullptr);
            return (_codec->Channels());
        }
        uint8_t BytesPerSample() const
        {
            return (2); /* always 16-bit samples! */
        }
        uint16_t MinFrameSize() const
        {
            ASSERT(_codec != nullptr);
            return (_codec->InFrameSize());
        }
        uint16_t PreferredFrameSize() const
        {
            ASSERT(_codec != nullptr);
            return ((A2DP_OMTU / _codec->OutFrameSize()) * _codec->InFrameSize());
        }
        void Reset()
        {
            _timestamp = 0;
            // Ideally the sequence should start with a random value...
            _sequence = (Core::Time::Now().Ticks() & 0xFFFF);
        }
        uint32_t Transmit(const uint16_t length /* in bytes! */, const uint8_t data[])
        {
            ASSERT(_codec != nullptr);

            uint32_t consumed = 0;

            MediaPacketType<A2DP_OMTU, A2DP_PAYLOAD_TYPE, IAudioCodec> packet(*_codec, _ssrc, _sequence, _timestamp);

            consumed = packet.Ingest(length, data);

            if (consumed > 0) {
                uint32_t result = Exchange(PacketTimeout, packet);
                if (result != Core::ERROR_NONE) {
                    fprintf(stderr, "BluetoothAudioSink: Failed to send out media packet (%d)\n", result);
                }

                // Timestamp clock frequency is the same as the sampling frequency.
                _timestamp += (consumed / (_codec->Channels() * BytesPerSample()));

                _sequence++;
            }

            return (consumed);
        }

    private:
        Core::CriticalSection _lock;
        IAudioCodec* _codec;
        uint8_t _ssrc;
        uint32_t _timestamp;
        uint16_t _sequence;
    }; // class TransportChannel

} // namespace A2DP

}


