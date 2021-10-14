/*
 * If not stated otherwise in this file or this component's LICENSE file the
 * following copyright and licenses apply:
 *
 * Copyright 2020 RDK Management
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
 
#include "Module.h"

#include <interfaces/IComposition.h>
#include <com/com.h>

#include "ModeSet.h"

#ifdef __cplusplus
extern "C" {
#endif

#include <gbm.h>
#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <GLES2/gl2.h>
#include <GLES2/gl2ext.h>
#include <sys/socket.h>

#ifdef __cplusplus
}
#endif

#include <mutex>
#include <type_traits>
#include <functional>
#include <chrono>

template <class T>
struct remove_pointer {
    typedef T type;
};

template <class T>
struct remove_pointer <T*> {
    typedef T type;
};

template <class T>
struct remove_pointer <T const *>  {
    typedef T type;
};

template <class T>
struct _remove_const {
    typedef T type;
};

template <class T>
struct _remove_const <T const> {
    typedef T type;
};

template <class T>
struct _remove_const <T *> {
    typedef T * type;
};

template <class T>
struct _remove_const <T const *> {
    typedef T * type;
};

template <class FROM, class TO, bool ENABLE>
struct _narrowing {
    static_assert (( std::is_arithmetic < FROM > :: value && std::is_arithmetic < TO > :: value ) != false);

    // Not complete, assume zero is minimum for unsigned
    // Common type of signed and unsigned typically is unsigned
    using common_t = typename std::common_type < FROM, TO > :: type;
    static constexpr bool value =   ENABLE
                                    && (
                                        ( std::is_signed < FROM > :: value && std::is_unsigned < TO > :: value )
                                        || static_cast < common_t > ( std::numeric_limits < FROM >::max () ) >= static_cast < common_t > ( std::numeric_limits < TO >::max () )
                                    )
                                    ;
};

// Suppress compiler warnings of unused (parameters)
// Omitting the name is sufficient but a search on this keyword provides easy access to the location
template <typename T>
void silence (T &&) {}

// Show logging in blue to make it distinct from TRACE
#define TRACE_WITHOUT_THIS(level, ...) do {             \
    fprintf (stderr, "\033[1;34m");                     \
    fprintf (stderr, "[%s:%d] : ", __FILE__, __LINE__); \
    fprintf (stderr, ##__VA_ARGS__);                    \
    fprintf (stderr, "\n");                             \
    fprintf (stderr, "\033[0m");                        \
    fflush (stderr);                                    \
} while (0)


MODULE_NAME_DECLARATION(BUILD_REFERENCE)

namespace WPEFramework {
namespace Plugin {

class CompositorImplementation;

    // Some compilers might struggle with identical names for class and namespace, but for now it simplifies a lot
    namespace EGL {
#ifdef EGL_VERSION_1_5
        using img_t = EGLImage;
#else
        using img_t = EGLImageKHR;
#endif
        using width_t = EGLint;
        using height_t = EGLint;

#ifdef EGL_VERSION_1_5
        static constexpr img_t InvalidImage () { return EGL_NO_IMAGE; }
#else
        static constexpr img_t InvalidImage () { return EGL_NO_IMAGE_KHR; }
#endif
    }

    class ClientSurface : public Exchange::IComposition::IClient, Exchange::IComposition::IRender {
    private:

        // The buffer acts as a surface for the remote site
        struct {
            struct gbm_bo * _buf;
            int _fd;
            EGL::img_t _khr;

            bool Valid () const { return _buf != nullptr; }
            bool DMAComplete () const { return Valid () && _fd > -1; };
            bool RenderComplete () const { return Valid () && _fd > -1 && _khr != WPEFramework::Plugin::EGL::InvalidImage (); }
        } _nativeSurface;

    public:

        using surf_t = decltype (_nativeSurface);

    public:
        ClientSurface() = delete;
        ClientSurface(const ClientSurface&) = delete;
        ClientSurface& operator= (const ClientSurface&) = delete;

        ClientSurface(ModeSet& modeSet, CompositorImplementation& compositor, const string& name, const uint32_t width, const uint32_t height);
        ~ClientSurface() override;

    public:
        RPC::instance_id Native () const override {
            // Sharing this handle does not imply its contents can be accessed!

            static_assert ((std::is_convertible < decltype (_nativeSurface._fd), RPC::instance_id > :: value) != false);
            static_assert (sizeof (decltype (_nativeSurface._fd) ) <= sizeof (RPC::instance_id));

            return static_cast < RPC::instance_id > ( _nativeSurface._fd );
        }

        string Name() const override
        {
                return _name;
        }
        void Opacity(const uint32_t value) override {
            _opacity = value;
        }
        uint32_t Opacity () const override {
            return _opacity;
        }
        uint32_t Geometry(const Exchange::IComposition::Rectangle& rectangle) override {
            _destination = rectangle;
            return Core::ERROR_NONE;
        }
        Exchange::IComposition::Rectangle Geometry() const override {
            return _destination;
        }
        uint32_t ZOrder(const uint16_t zorder) override {
            _layer = zorder;
            return Core::ERROR_NONE;
        }
        uint32_t ZOrder() const override {
            return _layer;
        }

        void ScanOut () override;

        surf_t const & Surface ( EGL::img_t const & khr = EGL::InvalidImage () ) {
            if (khr != EGL::InvalidImage ()) {
                _nativeSurface._khr = khr;
            }

            return _nativeSurface;
        }

        BEGIN_INTERFACE_MAP (ClientSurface)
            INTERFACE_ENTRY (Exchange::IComposition::IClient)
            INTERFACE_ENTRY (Exchange::IComposition::IRender)
        END_INTERFACE_MAP

    private:
        ModeSet& _modeSet;
        CompositorImplementation& _compositor; 
        const std::string _name;

        uint32_t _opacity;
        uint32_t _layer;

        Exchange::IComposition::Rectangle _destination;
    };

    class CompositorImplementation : public Exchange::IComposition, public Exchange::IComposition::IDisplay {
    private:
        CompositorImplementation(const CompositorImplementation&) = delete;
        CompositorImplementation& operator=(const CompositorImplementation&) = delete;

        class ExternalAccess : public RPC::Communicator {
        private:
            ExternalAccess() = delete;
            ExternalAccess(const ExternalAccess&) = delete;
            ExternalAccess& operator=(const ExternalAccess&) = delete;

        public:
            ExternalAccess(
                CompositorImplementation& parent, 
                const Core::NodeId& source, 
                const string& proxyStubPath, 
                const Core::ProxyType<RPC::InvokeServer>& handler)
                : RPC::Communicator(source,  proxyStubPath.empty() == false ? Core::Directory::Normalize(proxyStubPath) : proxyStubPath, Core::ProxyType<Core::IIPCServer>(handler))
                , _parent(parent)
            {
                uint32_t result = RPC::Communicator::Open(RPC::CommunicationTimeOut);

                handler->Announcements(Announcement());

                if (result != Core::ERROR_NONE) {
                    TRACE(Trace::Error, (_T("Could not open RPI Compositor RPCLink server. Error: %s"), Core::NumberType<uint32_t>(result).Text()));
                } else {
                    // We need to pass the communication channel NodeId via an environment variable, for process,
                    // not being started by the rpcprocess...
                    Core::SystemInfo::SetEnvironment(_T("COMPOSITOR"), RPC::Communicator::Connector(), true);
                }
            }

            ~ExternalAccess() override = default;

        private:
            void* Aquire(const string& className, const uint32_t interfaceId, const uint32_t version) override
            {
                // Use the className to check for multiple HDMI's. 
                return (_parent.QueryInterface(interfaceId));
            }

        private:
            CompositorImplementation& _parent;
        };

        class DMATransfer : public Core::Thread {

            private :

                CompositorImplementation & _compositor;

                // Waiting for connection requests
                int _listen;
                // Actual socket for communication
                int _transfer;

                struct sockaddr_un const _addr;

                bool _valid;

                using valid_t = decltype (_valid);

            public :

                DMATransfer () = delete;
                DMATransfer (CompositorImplementation & compositor) : Core::Thread (/*0, _T ("")*/), _compositor (compositor), _listen { -1 }, _transfer { -1 }, _addr { AF_UNIX, "/tmp/Compositor/DMA" }, _valid { _Initialize () } {}
                ~DMATransfer () {
                    Stop ();
                    /* valid_t */ _Deinitialize ();
                }

                DMATransfer (DMATransfer const &) = delete;
                DMATransfer & operator = (DMATransfer const &) = delete;

            private :

                using timeout_t = _remove_const < decltype (Core::infinite) > :: type;

            public :

                timeout_t Worker () override {
                    // Never call 'us' again, delay the next call an infinite amount of time if the state is not 'stopped'
                    timeout_t _ret = Core::infinite;

                    if (IsRunning () != false) {

                        // Blocking
                        _transfer = accept (_listen, nullptr, nullptr);

                        // Do some processing on the clients

                        std::string _msg;
                        int _fd = -1;

                        std::string _props;

                        if (Receive (_msg, _fd) && _compositor.FDFor (_msg, _fd, _props) && Send (_msg + _props, _fd) != false) {
                            // Just wait for the remote peer to close the connection
                            ssize_t _size = read (_transfer, nullptr, 0);

                            decltype (errno) _err = errno;

                            switch (_size) {
                                case -1 :   // Error
                                            TRACE (Trace::Error, (_T ("Error after DMA transfer : %d."), _err));
                                            break;
                                case 0  :   // remote has closed the connection
                                            TRACE (Trace::Information, (_T ("Remote has closed the DMA connection.")));
                                            break;
                                default :   // Unexpected data available
                                            TRACE (Trace::Error, (_T ("Unexpected data read after DMA transfer.")));
                            }

                            /* int */ close (_transfer);

                            _transfer = -1;
                        }
                        else {
                            TRACE (Trace::Error, (_T ("Failed to exchange DMA information for %s."),  _msg.length () > 0 ? _msg.c_str () : "'<no name provided>'"));
                        }
                    }

                    return _ret;
                }

                valid_t Valid () const { return _valid; }

                // Receive file descriptor with additional message
                valid_t Receive (std::string & msg, int & fd) {
                    valid_t _ret = Valid () && Connect (Core::infinite);

                    if (_ret != true) {
                        TRACE (Trace::Information, (_T ("Unable to receive (DMA) data.")));
                    }
                    else {
                        _ret = _Receive (msg, fd);
                        _ret = Disconnect (Core::infinite) && _ret;
                    }

                    return _ret;
                }

                // Send file descriptor with additional message
                valid_t Send (std::string const & msg, int fd) {
                    valid_t _ret = Valid () && Connect (Core::infinite);

                    if (_ret != true) {
                        TRACE (Trace::Information, (_T ("Unable to send (DMA) data.")));
                    }
                    else {
                        _ret = _Send (msg, fd) && Disconnect (Core::infinite);
                        _ret = Disconnect (Core::infinite) && _ret;
                    }

                    return _ret;
                }

            private :

                valid_t _Initialize () {
                    valid_t _ret = false;

                    // Just a precaution
                    /* int */ unlink (_addr.sun_path);

                    _listen = socket (_addr.sun_family /* domain */, SOCK_STREAM /* type */, 0 /* protocol */);
                    _ret = _listen != -1;

                    if (_ret != false) {
                        _ret = bind (_listen, reinterpret_cast < struct sockaddr const * > (&_addr), sizeof (_addr)) == 0;
                    }

                    if (_ret != false) {
// TODO: Derive it from maximum number of supported clients
                        // Number of pending requests for accept to handle
                        constexpr int queue_size = 1;
                        _ret = listen (_listen, queue_size) == 0;
                    }

                    return _ret;
                }

                valid_t _Deinitialize () {
                    valid_t _ret = false;

                    _ret = close (_listen) == 0 && close (_transfer) == 0;

                    // Delete the (bind) socket in the file system if no reference exist (anymore)
                    _ret = unlink (_addr.sun_path) == 0 && _ret;

                    return _ret;
                }

                valid_t Connect (timeout_t timeout) {
                    silence (timeout);

                    valid_t _ret = false;

                    decltype (errno) _err = errno;

                    _ret = _transfer > -1 && _err == 0;

                    return _ret;
                }

                valid_t Disconnect (timeout_t timeout) {
                    silence (timeout);

                    valid_t _ret = false;

                    decltype (errno) _err = errno;

                    _ret = _transfer > -1 && _err == 0;

                    return _ret;
                }

                valid_t _Send (std::string const & msg, int fd) {
                    valid_t _ret = false;

                    // Logical const
                    static_assert ((std::is_same <char *, _remove_const  < decltype ( & msg [0] ) > :: type >:: value) != false);
                    char * _buf  = const_cast <char *> ( & msg [0] );

                    size_t const _bufsize = msg.size ();

                    if (_bufsize > 0) {

                        // Scatter array for vector I/O
                        struct iovec _iov;

                        // Starting address
                        _iov.iov_base = reinterpret_cast <void *> (_buf);
                        // Number of bytes to transfer
                        _iov.iov_len = _bufsize;

                        // Actual message
                        struct msghdr _msgh {};

                        // Optional address
                        _msgh.msg_name = nullptr;
                        // Size of address
                        _msgh.msg_namelen = 0;
                        // Scatter array
                        _msgh.msg_iov = &_iov;
                        // Elements in msg_iov
                        _msgh.msg_iovlen = 1;

                        // Ancillary data
                        // The macro returns the number of bytes an ancillary element with payload of the passed in data length, eg size of ancillary data to be sent
                        char _control [CMSG_SPACE (sizeof ( decltype ( fd) ))];

                        if (fd > -1) {
                            // Contruct ancillary data to be added to the transfer via the control message

                            // Ancillary data, pointer
                            _msgh.msg_control = _control;

                            // Ancillary data buffer length
                            _msgh.msg_controllen = sizeof ( _control );

                            // Ancillary data should be access via cmsg macros
                            // https://linux.die.net/man/2/recvmsg
                            // https://linux.die.net/man/3/cmsg
                            // https://linux.die.net/man/2/setsockopt
                            // https://www.man7.org/linux/man-pages/man7/unix.7.html

                            // Control message

                            // Pointer to the first cmsghdr in the ancillary data buffer associated with the passed msgh
                            struct cmsghdr * _cmsgh = CMSG_FIRSTHDR ( &_msgh );

                            if (_cmsgh != nullptr) {
                                // Originating protocol
                                // To manipulate options at the sockets API level
                                _cmsgh->cmsg_level = SOL_SOCKET;

                                // Protocol specific type
                                // Option at the API level, send or receive a set of open file descriptors from another process
                                _cmsgh->cmsg_type = SCM_RIGHTS;

                                // The value to store in the cmsg_len member of the cmsghdr structure, taking into account any necessary alignmen, eg byte count of control message including header
                                _cmsgh->cmsg_len = CMSG_LEN (sizeof ( decltype ( fd ) ));

                                // Initialize the payload
                                // Pointer to the data portion of a cmsghdr, ie unsigned char []
                                * reinterpret_cast < decltype (fd) * > ( CMSG_DATA ( _cmsgh ) ) = fd;

                                _ret = true;
                            }
                            else {
                                // Error
                            }
                        }
                        else {
                            // No extra payload, ie  file descriptor(s), to include
                            _msgh.msg_control = nullptr;
                            _msgh.msg_controllen = 0;

                            _ret = true;
                        }

                        if (_ret != false) {
                            // Configuration succeeded

                            // https://linux.die.net/man/2/sendmsg
                            // https://linux.die.net/man/2/write
                            // Zero flags is equivalent to write

                            ssize_t _size = -1;
                            socklen_t _len = sizeof (_size);

                            // Only send data if the buffer is large enough to contain all data
                            if (getsockopt (_transfer, SOL_SOCKET, SO_SNDBUF, &_size, &_len) == 0) {
                                // Most options use type int, ssize_t was just a placeholder
                                static_assert (sizeof (int) <= sizeof (ssize_t));
                                TRACE (Trace::Information, (_T ("The sending buffer capacity equals %d bytes."), _size));

// TODO: do not send if the sending buffer is too small
                                _size = sendmsg (_transfer, &_msgh, 0);
                            }
                            else {
                                // Unable to determine buffer capacity
                            }

                            _ret = _size != -1;

                            if (_ret != false) {
                                // Ancillary data is not included
                                TRACE (Trace::Information, (_T ("Send %d bytes out of %d."), _size, _bufsize));
                            }
                            else {
                                TRACE (Trace::Error, (_T ("Failed to send data.")));
                            }
                        }

                    }
                    else {
                        TRACE (Trace::Error, (_T ("A data message to be send cannot be empty!")));
                    }

                    return _ret;
                }

                valid_t _Receive (std::string & msg, int & fd) {
                    bool _ret = false;

                    msg.clear ();
                    fd = -1;

                    ssize_t _size = -1;
                    socklen_t _len = sizeof (_size);

                    if (getsockopt (_transfer, SOL_SOCKET, SO_RCVBUF, &_size, &_len) == 0) {
                        TRACE (Trace::Information, (_T ("The receiving buffer maximum capacity equals %d bytes."), _size));

                        // Most options use type int, ssize_t was just a placeholder
                        static_assert (sizeof (int) <= sizeof (ssize_t));
                        msg.reserve (static_cast < int > (_size));
                    }
                    else {
                        TRACE (Trace::Information, (_T ("Unable to determine buffer maximum cpacity. Using %d bytes instead."), msg.capacity ()));
                    }

                    size_t const _bufsize = msg.capacity ();

                    if (_bufsize > 0) {
                        using fd_t = std::remove_reference < decltype (fd) > :: type;

                        static_assert ((std::is_same <char *, _remove_const  < decltype ( & msg [0] ) > :: type >:: value) != false);
                        char _buf [_bufsize];

                        // Scatter array for vector I/O
                        struct iovec _iov;

                        // Starting address
                        _iov.iov_base = reinterpret_cast <void *> (&_buf [0]);
                        // Number of bytes to transfer
                        _iov.iov_len = _bufsize;

                        // Actual message
                        struct msghdr _msgh {};

                        // Optional address
                        _msgh.msg_name = nullptr;
                        // Size of address
                        _msgh.msg_namelen = 0;
                        // Elements in msg_iov
                        _msgh.msg_iovlen = 1;
                        // Scatter array
                        _msgh.msg_iov = &_iov;

                        // Ancillary data
                        // The macro returns the number of bytes an ancillary element with payload of the passed in data length, eg size of ancillary data to be sent
                        char _control [CMSG_SPACE (sizeof ( fd_t ))];

                        // Ancillary data, pointer
                        _msgh.msg_control = _control;

                        // Ancillery data buffer length
                        _msgh.msg_controllen = sizeof (_control);

// TODO: do not receive if the receiving buffer is too small
                        // No flags set
                        _size = recvmsg (_transfer, &_msgh, 0);

                        _ret = _size > 0;

                        switch (_size) {
                            case -1 :   // Error
                                        {
                                           TRACE (Trace::Error, (_T ("Error receiving remote (DMA) data.")));
                                           break;
                                        }
                            case 0  :   // Peer has done an orderly shutdown, before any data was received
                                        {
                                            TRACE (Trace::Error, (_T ("Error receiving remote (DMA) data. Compositorclient may have become unavailable.")));
                                            break;
                                        }
                            default :   // Data
                                        {
                                            // Extract the file descriptor information
                                            TRACE (Trace::Information, (_T ("Received %d bytes."), _size));

                                            // Pointer to the first cmsghdr in the ancillary data buffer associated with the passed msgh
                                            // Assume a single cmsgh was sent
                                            struct cmsghdr * _cmsgh = CMSG_FIRSTHDR ( &_msgh );

                                            // Check for the expected properties the client should have set
                                            if (_cmsgh != nullptr
                                                && _cmsgh->cmsg_level == SOL_SOCKET
                                                && _cmsgh->cmsg_type == SCM_RIGHTS) {

                                                // The macro returns a pointer to the data portion of a cmsghdr.
                                                fd = * reinterpret_cast < fd_t * > ( CMSG_DATA ( _cmsgh ) );
                                            }
                                            else {
                                                TRACE (Trace::Information, (_T ("No (valid) ancillary data received.")));
                                            }

                                            msg.assign (_buf, _size);
                                        }
                        }
                    }
                    else {
                        // Error
                        TRACE (Trace::Error, (_T ("A receiving data buffer (message) cannot be empty!")));
                    }

                    return _ret;
                }
        };

        class Natives {
            private :

                ModeSet & _set;

                using dpy_return_t = decltype ( std::declval < ModeSet > ().UnderlyingHandle () );
                using surf_return_t = decltype ( std::declval < ModeSet > ().CreateRenderTarget (0, 0) );

                static_assert (std::is_pointer < surf_return_t > :: value != false);
                surf_return_t _surf = nullptr;

                bool _valid = false;

            public :

                using dpy_t = dpy_return_t;
                using surf_t = decltype (_surf);
                using valid_t = decltype (_valid);

                Natives () = delete;
                Natives (ModeSet & set) : _set { set }, _valid { Initialize () } {}
                ~Natives () {
                    _valid = false;
                    DeInitialize ();
                }

                dpy_t Display () const { return _set.UnderlyingHandle (); }
                surf_t Surface () const { return _surf; }

                valid_t Valid () const { return _valid; }

            private :

                valid_t Initialize () {
                    valid_t _ret = false;

                    // The argument to Open is unused, an empty string suffices
                    static_assert (std::is_pointer < dpy_t > ::value != false);
                    _ret =_set.Open ("") == Core::ERROR_NONE && Display () != nullptr;

                    using width_t = decltype ( std::declval < ModeSet > ().Width () );
                    using height_t = decltype ( std::declval < ModeSet > ().Height () );

                    width_t _width = _set.Width ();
                    height_t _height = _set.Height ();

                    if (_ret != false) {
                        _surf = _set.CreateRenderTarget (_width, _height);
                        _ret = _surf != nullptr;
                    }

                    if (_ret != true) {
                        static_assert (( std::is_integral < width_t >::value && std::is_integral < height_t >::value ) != false);
                        TRACE (Trace::Error, (_T ("Unable to create a compositor surface of dimensions: %d x %d [width, height]))."), _width, _height));
                    }

                    return _ret;
                }

                void DeInitialize () {
                    _valid = false;

                    if (_surf != nullptr) {
                        _set.DestroyRenderTarget (_surf);
                    }
                }
        };

        class GLES {
            private :

                // x, y, z
                static constexpr uint8_t VerticeDimensions = 3;

                struct offset {
                    using coordinate_t = GLfloat;

                    static_assert (_narrowing <float, coordinate_t, true> :: value != false);

                    // Each coorrdinate in the range [-1.0f, 1.0f]
                    static constexpr coordinate_t _left = static_cast <coordinate_t> (-1.0f);
                    static constexpr coordinate_t _right = static_cast <coordinate_t> (1.0f);
                    static constexpr coordinate_t _bottom = static_cast <coordinate_t> (-1.0f);
                    static constexpr coordinate_t _top = static_cast <coordinate_t> (1.0f);
                    static constexpr coordinate_t _near = static_cast <coordinate_t> (-1.0f);
                    static constexpr coordinate_t _far = static_cast <coordinate_t> (1.0f);

                    coordinate_t _x;
                    coordinate_t _y;
                    coordinate_t _z;

                    offset () : offset ( (_right - _left) / static_cast <coordinate_t> (2.0f) + _left, (_top - _bottom) / static_cast <coordinate_t> (2.0f) + _bottom, (_far - _near) / static_cast <coordinate_t> (2.0f) + _near ) {}
                    offset (coordinate_t x, coordinate_t y, coordinate_t z) : _x {x}, _y {y}, _z {z} {}
                } _offset;

                struct scale {
                    using fraction_t = GLclampf;

                    static_assert (_narrowing <float, fraction_t, true> :: value != false);

                    static constexpr fraction_t _identity = static_cast <fraction_t> (1.0f);
                    static constexpr fraction_t _min  = static_cast <fraction_t> (0.0f);
                    static constexpr fraction_t _max  = static_cast <fraction_t> (1.0f);

                    fraction_t _horiz;
                    fraction_t _vert;

                    scale () : scale (_identity, _identity) {}
                    scale (fraction_t horiz, fraction_t vert) : _horiz {horiz}, _vert {vert} {}
                } _scale;

                struct opacity {
                    using alpha_t = GLfloat;

                    static_assert (_narrowing <float, alpha_t, true> :: value != false);

                    static constexpr alpha_t _min = static_cast <alpha_t> (0.0f);
                    static constexpr alpha_t _max = static_cast <alpha_t> (1.0f);

                    alpha_t _alpha;;

                    opacity () : opacity {_max} {}
                    opacity (alpha_t alpha) : _alpha {alpha} {}
                } _opacity;

                bool _valid;

            public :

                using tgt_t = GLuint;
                using tex_t = GLuint;
                using offset_t = decltype (_offset);
                using scale_t = decltype (_scale);
                using opacity_t = decltype (_opacity);

                using width_t = GLuint;
                using height_t = GLuint;

                using version_t = GLuint;

                using valid_t = decltype (_valid);

            private :

                struct texture {
                    tex_t _tex;
                    tgt_t _target;

                    offset_t _offset;
                    scale_t _scale;
                    opacity_t _opacity;

                    width_t _width;
                    height_t _height;

                    texture () : texture { GL_INVALID_ENUM, GLES::InitialOffset (), GLES::InitialScale (), GLES::InitialOpacity () } {}
                    texture (tgt_t target, offset_t offset, scale_t scale, opacity_t opacity) : _tex {0}, _target {target}, _offset {offset}, _scale {scale}, _opacity {opacity}, _width {0}, _height {0} {}
                    texture (texture const & other) : _tex { other._tex },_target { other._target }, _offset { other._offset }, _scale { other._scale }, _opacity { other._opacity }, _width { other._width }, _height  { other._height } {}
                };

            public :

                using texture_t = struct texture;

            private :

                std::map <EGL::img_t, texture_t> _scene;
                std::mutex _token;

            public :

                GLES () : _offset { InitialOffset () }, _scale {InitialScale () }, _opacity { InitialOpacity () },  _valid { Initialize () } {}
                ~GLES () {
                    _valid = false;

                    /* valid_t */ Deinitialize ();
                }

                static offset_t InitialOffset () { return offset (); }

                valid_t UpdateOffset (offset_t const & off) {
                    valid_t _ret = false;

                    // Ramge check without taking into account rounding errors
                    if (   off._x >= offset_t::_left
                        && off._x <= offset_t::_right
                        && off._y >= offset_t::_bottom
                        && off._y <= offset_t::_top
                        && off._z >= offset_t::_near
                        && off._z <= offset_t::_far
                    )
                    {
                        _offset = off;
                        _ret = true;
                    }

                    return _ret;
                }

                static scale_t InitialScale () { return scale (); }

                valid_t UpdateScale (scale_t const & scale) {
                    valid_t _ret = false;

                    // Ramge check without taking into account rounding errors
                    if (   scale._horiz >= scale_t::_min
                        && scale._horiz <= scale_t::_max
                        && scale._vert >= scale_t::_min
                        && scale._vert <= scale_t::_max
                    )
                    {
                        _scale = scale;
                        _ret = true;
                    }

                    return _ret;
                }

                static opacity_t InitialOpacity () { return opacity (); }

                valid_t UpdateOpacity (opacity_t const & opacity) {
                    valid_t _ret = false;

                    if (   opacity._alpha >= opacity_t::_min
                        && _opacity._alpha <= opacity_t::_max
                    )
                    {
                        _opacity = opacity;
                        _ret = true;
                    }

                    return _ret;
                }

                static constexpr tex_t InvalidTex () { return 0; }

                static constexpr version_t MajorVersion () { return static_cast <version_t> (2); }
                static constexpr version_t MinorVersion () { return static_cast <version_t> (0); }

                valid_t Valid () const { return _valid; }

                valid_t Render () {
                    bool _ret = Valid ();
                    return _ret;
                }

                valid_t RenderColor (bool red, bool green, bool blue) {
                    static uint16_t _degree = 0;

                    constexpr decltype (_degree) const ROTATION = 360;

                    constexpr float const OMEGA = 3.14159265 / 180;

                    valid_t  _ret = Valid ();

                    if (_ret != false) {
                        // Here, for C(++) these type should be identical
                        // Type information: https://www.khronos.org/opengl/wiki/OpenGL_Type
                        static_assert (std::is_same <float, GLfloat>::value);

                        GLfloat _rad = static_cast <GLfloat> (cos (_degree * OMEGA));

                        constexpr GLfloat _default_color = 0.0f;

                        // The function clamps the input to [0.0f, 1.0f]
                        /* void */ glClearColor (red != false ? _rad : _default_color, green != false ? _rad : _default_color, blue != false ? _rad : _default_color, 1.0);

                        _ret = glGetError () == GL_NO_ERROR;

                        if (_ret != false) {
                            /* void */ glClear (GL_COLOR_BUFFER_BIT);
                            _ret = glGetError () == GL_NO_ERROR;
                        }

                        if (_ret != false) {
                            /* void */ glFlush ();
                            _ret = glGetError () == GL_NO_ERROR;
                        }

                        _degree = (_degree + 1) % ROTATION;
                    }

                    return _ret;
                }

                valid_t SkipEGLImageFromScene (EGL::img_t const & img) {
                    valid_t _ret = false;

                    std::lock_guard < decltype (_token) > const lock (_token);

                    auto _it = _scene.find (img);

                    _ret = _it != _scene.end ();

                    if (_ret != false) {
                        using scene_t = decltype (_scene);

                        scene_t::size_type _size = _scene.size ();

                        _scene.erase (_it);

                        _ret = ( _size - _scene.size () ) == static_cast < scene_t::size_type > (1);
                    }

                    return _ret;
                }

                valid_t RenderEGLImage (EGL::img_t const & img, EGL::width_t width, EGL::height_t height) {
                    EGLDisplay _dpy = EGL::InvalidDpy ();
                    EGLDisplay _ctx = EGL::InvalidCtx ();

                    auto DestroyTexture = [this] (texture_t & tex) -> valid_t {
                        tex_t & _tex = tex._tex;

                        valid_t _ret = _tex != InvalidTex ();

                        // Delete the previously created texture
                        if (_ret != false) {
                            glDeleteTextures (1, &_tex);

                            _ret = glGetError () == GL_NO_ERROR;
                        }

                        if (_ret != false) {
                            _tex = InvalidTex ();
                        }

                        return _ret;
                    };

                    auto SetupTexture = [this, &_dpy, &_ctx] (texture_t & tex, EGL::img_t const & img, EGL::width_t width, EGL::height_t height) -> valid_t {
                        valid_t _ret = glGetError () == GL_NO_ERROR;

                        tex_t & _tex = tex._tex;
                        tgt_t & _tgt = tex._target;

                        if (_ret != false) {
                            glGenTextures (1, &_tex);
                            _ret = glGetError () == GL_NO_ERROR;
                        }


                        if (_ret != false) {
                            glBindTexture (_tgt, _tex);
                            _ret = glGetError () == GL_NO_ERROR;
                        }

                        if (_ret != false) {
                            glTexParameteri (_tgt, GL_TEXTURE_WRAP_S,GL_CLAMP_TO_EDGE);
                            _ret = glGetError () == GL_NO_ERROR;
                        }

                        if (_ret != false) {
                            glTexParameteri (_tgt, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
                            _ret = glGetError () == GL_NO_ERROR;
                        }

                        if (_ret != false) {
                            glTexParameteri (_tgt, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
                            _ret = glGetError () == GL_NO_ERROR;
                        }

                        if (_ret != false) {
                            glTexParameteri (_tgt, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
                            _ret = glGetError () == GL_NO_ERROR;
                        }

                        if (_ret != false) {
                            tex._width = width;
                            tex._height = height;

                            switch (_tgt) {
                                case GL_TEXTURE_EXTERNAL_OES :
                                    {
                                        // A valid GL context should exist for GLES::Supported ()

                                        _ret = _dpy != EGL::InvalidDpy () && _ctx != EGL::InvalidCtx ();

// TODO: do not check every pass
                                        if ( _ret && ( GLES::Supported ("GL_OES_EGL_image") && ( EGL::Supported (_dpy, "EGL_KHR_image") || EGL::Supported (_dpy, "EGL_KHR_image_base") ) ) != false) {
                                            // Take storage for the texture from the EGLImage; Pixel data becomes undefined
                                            static void (* _EGLImageTargetTexture2DOES) (GLenum, GLeglImageOES) = reinterpret_cast < void (*) (GLenum, GLeglImageOES) > (eglGetProcAddress ("glEGLImageTargetTexture2DOES"));

                                            _ret = _EGLImageTargetTexture2DOES != nullptr;

                                            if (_ret != false) {
                                                // Logical const
                                                using no_const_img_t = _remove_const < decltype (img) > :: type;

                                                _EGLImageTargetTexture2DOES (_tgt, reinterpret_cast <GLeglImageOES> (const_cast < no_const_img_t  >(img)));

                                                _ret = glGetError () == GL_NO_ERROR;
                                            }
                                        }
                                    }; break;

                                case GL_TEXTURE_2D :
                                    {
                                        glTexImage2D (GL_TEXTURE_2D, 0, GL_RGBA, tex._width, tex._height, 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr); 

                                        _ret = glGetError () == GL_NO_ERROR;

                                    }; break;

                                default :
                                    {
                                        _ret = false;
                                    }
                            }
                        }


                        if (_ret != false) {
                            glBindTexture (_tgt, InvalidTex ());
                            _ret = glGetError () == GL_NO_ERROR;
                        }

                        return _ret;
                    };


                    valid_t _ret = glGetError () == GL_NO_ERROR && img != EGL::InvalidImage () && width > 0 && height > 0;

                    // A valid GL context should exist for GLES::Supported ()
                    /* EGL::ctx_t */ _ctx = eglGetCurrentContext ();
                    /* EGL::dpy_t */ _dpy = _ctx != EGL::InvalidCtx () ? eglGetCurrentDisplay () : EGL::InvalidDpy ();

                    _ret = _ret && eglGetError () == EGL_SUCCESS && _ctx != EGL::InvalidCtx ();

                    EGLSurface _surf = EGL::InvalidSurf ();

                    if (_ret != false) {
                        _surf = eglGetCurrentSurface (EGL_DRAW);
                        _ret  = eglGetError () == EGL_SUCCESS
                                && _surf != EGL::InvalidSurf ();
                    }

                    EGLint _width = 0, _height = 0;

                    if (_ret != false) {
                        _ret = eglQuerySurface (_dpy, _surf, EGL_WIDTH, &_width) != EGL_FALSE
                               && eglQuerySurface (_dpy, _surf, EGL_HEIGHT, &_height) != EGL_FALSE
                               && eglGetError () == EGL_SUCCESS;
                    }


                    // Set up the required textures

                    // The  'shared' texture
                    texture_t _tex_oes (GL_TEXTURE_EXTERNAL_OES, GLES::InitialOffset (), GLES::InitialScale (), GLES::InitialOpacity ());

                    // The 'scene' texture
                    texture_t _tex_fbo (GL_TEXTURE_2D, GLES::InitialOffset (), GLES::InitialScale (), GLES::InitialOpacity ());


                    if (_ret != false) {
                        // Just an arbitrarily selected texture unit
                        glActiveTexture (GL_TEXTURE0);
                        _ret = glGetError () == GL_NO_ERROR;
                    }

                    if (_ret != false) {
                        glBindTexture(_tex_oes._target, InvalidTex ());
                        _ret = glGetError () == GL_NO_ERROR;
                    }

                    if (_ret != false) {
                        glBindTexture(_tex_fbo._target, InvalidTex ());
                        _ret = glGetError () == GL_NO_ERROR;
                    }


                    if (_ret != false) {
                        _ret = SetupTexture (_tex_oes, img, width, height) != false;
                    }

                    {
                        std::lock_guard < decltype (_token) > const lock (_token);

                        if (_ret != false) {
                            auto _it = _scene.find (img);

                            _ret = _it != _scene.end ();

                            if (_ret != false) {
                                // Found, just update values
                                _tex_fbo = _it->second;
                            }
                            else {
// TODO: optmize for true (target) size
                                _ret = SetupTexture (_tex_fbo, img, width, height) != false;

                                if (_ret != false) {
                                    auto _it = _scene.insert ( std::pair <EGL::img_t, texture_t> (img, _tex_fbo));

                                    _ret = _it.second != false;
                                }
                            }

                            // Update
                            if (_ret != false) {
                                _tex_fbo._offset = _offset;
                                _tex_fbo._scale = _scale;
                                _tex_fbo._opacity = _opacity;

                                _scene [img] = _tex_fbo;
                            }
                        }
                    }

                    GLuint _fbo;

                    if (_ret != false) {
                        glGenFramebuffers (1, &_fbo);
                        _ret = glGetError () == GL_NO_ERROR;
                    }

                    if (_ret != false) {
                        glBindFramebuffer (GL_FRAMEBUFFER, _fbo);
                        _ret = glGetError () == GL_NO_ERROR;
                    }

                    if (_ret != false) {
                        glBindTexture (_tex_oes._target, _tex_oes._tex);
                        _ret = glGetError () == GL_NO_ERROR;
                    }

                    if (_ret != false) {
                        glBindTexture (_tex_fbo._target, _tex_fbo._tex);
                        _ret = glGetError () == GL_NO_ERROR;
                    }


                    if (_ret != false) {
                        glFramebufferTexture2D (GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, _tex_fbo._tex, 0);;
                        _ret = glGetError () == GL_NO_ERROR;

                        if (_ret != false) {
                            GLenum _status = glCheckFramebufferStatus (GL_FRAMEBUFFER);
                            _ret = glGetError () == GL_NO_ERROR && _status == GL_FRAMEBUFFER_COMPLETE;;
                        }
                    }


                    if (_ret != false) {
                         glDisable (GL_DEPTH_TEST);
                        _ret = glGetError () == GL_NO_ERROR;
                    }

                    if (_ret != false) {
                         glDisable (GL_BLEND);
                        _ret = glGetError () == GL_NO_ERROR;
                    }


                    _ret = ( _ret && UpdateScale (_tex_oes._scale) && UpdateOffset (_tex_oes._offset) && UpdateOpacity (_tex_fbo._opacity) && SetupViewport (_width, _height) && RenderTileOES () ) != false;


                    if (_ret != false) {
                        glBindTexture (_tex_oes._target, InvalidTex ());
                        _ret = glGetError () == GL_NO_ERROR;
                    }

                    if (_ret != false) {
                        glBindTexture (_tex_fbo._target, InvalidTex ());
                        _ret = glGetError () == GL_NO_ERROR;
                    }


                    /* valid_t */ DestroyTexture (_tex_oes);
//                    // Do not destroy _text_fbo


                    return _ret;
                }

                valid_t RenderScene (GLES::width_t width, GLES::height_t height, std::function < GLES::valid_t (texture_t left,  texture_t right) > sortfunc) {
                    valid_t _ret = glGetError () == GL_NO_ERROR;


// TODO: very inefficient way to get z-order sorted textures
                    std::list <texture_t> _sorted;

                    {
                        std::lock_guard < decltype (_token) > const lock (_token);

                        for (auto _begin = _scene.begin (), _it = _begin, _end = _scene.end (); _it != _end; _it ++) {
                            _sorted.push_back (_it->second);
                        }
                    }

                    _sorted.sort (sortfunc);


                    if (_ret != false) {
                        if (_ret != false) {
                            glBindFramebuffer (GL_FRAMEBUFFER, 0);
                            _ret = glGetError () == GL_NO_ERROR;
                        }

                        if (_ret != false) {
                            GLenum _status = glCheckFramebufferStatus (GL_FRAMEBUFFER);
                            _ret = glGetError () == GL_NO_ERROR && _status == GL_FRAMEBUFFER_COMPLETE;;
                        }
                        else {
                            // Errorr
                        }
                    }

                    // Blend pixels with pixels already present in the frame buffer
                    if (_ret != false) {
                        if (_ret != false) {
                             glEnable (GL_BLEND);
                            _ret = glGetError () == GL_NO_ERROR;
                        }

                        if (_ret != false) {
                            glBlendEquationSeparate (GL_FUNC_ADD, GL_FUNC_ADD);
                            _ret = glGetError () == GL_NO_ERROR;
                        }

                        if (_ret != false) {
                            glBlendFuncSeparate (GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA, GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
                            _ret = glGetError () == GL_NO_ERROR;
                        }
                    }


                    if (_ret != false) {
                        if (_ret != false) {
                            GLint _bits = 0;

                            glGetIntegerv (GL_DEPTH_BITS, &_bits);

                            _ret = glGetError () == GL_NO_ERROR && _bits > static_cast <GLint> (0);
                        }

                        if (_ret != false) {
                            glEnable (GL_DEPTH_TEST);
                            _ret = glGetError () == GL_NO_ERROR;
                        }

                        if (_ret != false) {
                            glDepthMask (GL_TRUE);
                        }

                        if (_ret != false) {
                            glDepthFunc (GL_LESS);
                            _ret = glGetError () == GL_NO_ERROR;
                        }

                        if (_ret != false) {
                            // Fully utilize the depth buffer range
                            glDepthRangef (GLES::offset_t::_near, GLES::offset_t::_far);
                            _ret = glGetError () == GL_NO_ERROR;
                        }

                        if (_ret != false) {
// TODO: magic number
                            glClearDepthf (1.0f);
                            _ret = glGetError () == GL_NO_ERROR;
                        }

                        if (_ret != false) {
                            glClear (GL_DEPTH_BUFFER_BIT);
                            _ret = glGetError () == GL_NO_ERROR;
                        }
                    }

                    // Start with an empty (solid) background
                    if (_ret != false) {
                        _ret = RenderColor (false, false, false) != false;
                    }


                    // For all textures in map
                    if (_ret != false) {
                        offset_t _off = _offset;
                        scale_t _scl = _scale;
                        opacity_t _op = _opacity;

                        for (auto _begin = _sorted.begin (), _it = _begin, _end = _sorted.end (); _it != _end; _it ++) {
                            texture_t &_texture = *_it;

                            if (_ret != false) {
                                glBindTexture (_texture._target, _texture._tex);
                                _ret = glGetError () == GL_NO_ERROR;
                            }

                            // Update the offset, scale, depth to match _tex

                            _ret = ( _ret && UpdateScale (_texture._scale) && UpdateOffset (_texture._offset) && UpdateOpacity (_texture._opacity) && SetupViewport (width, height) && RenderTile () ) != false;

                            if (_ret != false) {
                                glBindTexture (_texture._target, InvalidTex ());
                                _ret = glGetError () == GL_NO_ERROR;
                            }

                            if (_ret != true) {
                                break;
                            }
                        }

                        _ret = ( _ret && UpdateScale (_scl) && UpdateOffset (_off) && UpdateOpacity (_op) ) != false;
                    }


                    // Unconditionally
                    glDisable (GL_DEPTH_TEST);
                    _ret = _ret && glGetError () == GL_NO_ERROR;

                    glDisable (GL_BLEND);
                    _ret = _ret && glGetError () == GL_NO_ERROR;


                    return _ret;
                }

            private :

                valid_t Initialize () {
                    valid_t _ret = false;
                    _ret = true;
                    return _ret;
                }

                valid_t Deinitialize () {
                    valid_t _ret = false;

                    glBindTexture (GL_TEXTURE_2D, InvalidTex ());
                    _ret = glGetError () == GL_NO_ERROR;

                    glBindTexture (GL_TEXTURE_EXTERNAL_OES, InvalidTex ());
                    _ret = _ret && glGetError () == GL_NO_ERROR;

                    std::lock_guard < decltype (_token) > const lock (_token);

                    for (auto _begin = _scene.begin (), _it = _begin, _end = _scene.end (); _it != _end; _it++) {
                        tex_t & _tex = _it->second._tex;
                        glDeleteTextures (1, &_tex);
                        _ret = _ret && glGetError () == GL_NO_ERROR;
                    }

                    return _ret;
                }

// TODO: precompile programs at initialization stage

                valid_t SetupProgram (char const vtx_src [], char const frag_src []) {
                    auto LoadShader = [] (GLuint type, GLchar const code []) -> GLuint {
                        valid_t _ret = glGetError () == GL_NO_ERROR;

                        GLuint _shader = 0;
                        if (_ret != false) {
                            _shader = glCreateShader (type);
                            _ret = glGetError () == GL_NO_ERROR;
                        }

                        if (_ret != false && _shader != 0) {
                            glShaderSource (_shader, 1, &code, nullptr);
                            _ret = glGetError () == GL_NO_ERROR;
                        }

                        if (_ret != false) {
                            glCompileShader (_shader);
                            _ret = glGetError () == GL_NO_ERROR;
                        }

                        if (_ret != false) {
                            GLint _status = GL_FALSE;

                            glGetShaderiv (_shader, GL_COMPILE_STATUS, &_status);

                            _ret = glGetError () == GL_NO_ERROR && _status != GL_FALSE;
                        }

                        if (_ret != true) {
                            GLint _size = 0;

                            glGetShaderiv (_shader, GL_INFO_LOG_LENGTH, &_size);

                            if (glGetError () == GL_NO_ERROR) {
                                GLchar _info [_size];
                                GLsizei _length = 0;

                                glGetShaderInfoLog (_shader, static_cast <GLsizei> (_size), &_length, &_info [0]);
//                                _ret = glGetError () == GL_NO_ERROR;

                                _info [_size] = '\0';

                                TRACE_WITHOUT_THIS (Trace::Error, _T ("Error: shader log: %s"), static_cast <char *> (&_info [0]));
                            }
                        }

                        return _shader;
                    };

                    auto ShadersToProgram = [] (GLuint vertex, GLuint fragment) -> valid_t {
                        valid_t _ret = glGetError () == GL_NO_ERROR;

                         GLuint _prog = 0;

                        if (_ret != false) {
                            _prog = glCreateProgram ();
                            _ret = _prog != 0;
                        }

                        if (_ret != false) {
                            glAttachShader (_prog, vertex);
                            _ret = glGetError () == GL_NO_ERROR;
                        }

                        if (_ret != false) {
                             glAttachShader (_prog, fragment);
                            _ret = glGetError () == GL_NO_ERROR;
                        }

                        if (_ret != false) {
                            glBindAttribLocation (_prog, 0, "position");
                            _ret = glGetError () == GL_NO_ERROR;
                        }

                        if (_ret != false) {
                            glLinkProgram (_prog);
                            _ret = glGetError () == GL_NO_ERROR;
                        }

                        if (_ret != false) {
                            GLint _status = GL_FALSE;

                            glGetProgramiv (_prog, GL_LINK_STATUS, &_status);

                            _ret = glGetError () == GL_NO_ERROR && _status != GL_FALSE;
                        }

                        if (_ret != true) {
                            GLint _size = 0;
                            glGetProgramiv (_prog, GL_INFO_LOG_LENGTH, &_size);

                            if (glGetError () == GL_NO_ERROR) {
                                GLchar _info [_size];
                                GLsizei _length = 0;

                                glGetProgramInfoLog (_prog, static_cast <GLsizei> (_size), &_length, &_info [0]);
//                                _ret = glGetError () == GL_NO_ERROR;

                                _info [_size] = '\0';

                                TRACE_WITHOUT_THIS (Trace::Error, _T ("Error: program log: %s"), static_cast <char *> (&_info [0]));
                            }
                        }

                        if (_ret != false) {
                            glUseProgram (_prog);
                            _ret = glGetError () == GL_NO_ERROR;
                        }

                        return _ret;
                    };

                    auto DeleteCurrentProgram = [] () -> valid_t {
                        valid_t _ret = glGetError () == GL_NO_ERROR;

                        GLuint _prog = 0;

                        if (_ret != false) {
                            glGetIntegerv (GL_CURRENT_PROGRAM, reinterpret_cast <GLint *> (&_prog));
                            _ret = glGetError () == GL_NO_ERROR;
                        }

                        if (_ret != false && _prog != 0) {

                            GLint _count = 0;

                            glGetProgramiv (_prog, GL_ATTACHED_SHADERS, &_count);
                            _ret = glGetError () == GL_NO_ERROR && _count > 0;

                            if (_ret != false) {
                                GLuint _shaders [_count];

                                glGetAttachedShaders (_prog, _count, static_cast <GLsizei *> (&_count), &_shaders [0]);
                                _ret = glGetError () == GL_NO_ERROR;

                                if (_ret != false) {
                                    for (_count--; _count >= 0; _count--) {
                                        glDetachShader (_prog, _shaders [_count]);
                                        _ret = _ret && glGetError () == GL_NO_ERROR;
                                        glDeleteShader (_shaders [_count]);
                                        _ret = _ret && glGetError () == GL_NO_ERROR;
                                    }
                                }

                                if (_ret != false) {
                                    glDeleteProgram (_prog);
                                    _ret = glGetError () == GL_NO_ERROR;
                                }
                            }

                        }

                        return _ret;
                    };


                    bool _ret = glGetError () == GL_NO_ERROR
                                && Supported ("GL_OES_EGL_image_external") != false
                                && DeleteCurrentProgram () != false;

                    if (_ret != false) {
                        GLuint _vtxShader = LoadShader (GL_VERTEX_SHADER, vtx_src);
                        GLuint _fragShader = LoadShader (GL_FRAGMENT_SHADER, frag_src);

// TODO: inefficient on every call, reuse compiled program
                        _ret = ShadersToProgram(_vtxShader, _fragShader);
                    }

                    // Color on error
                    if (_ret != true) {
                        glClearColor (1.0f, 0.0f, 0.0f, 0.5f);
                        //_ret = glGetError () == GL_NO_ERROR;
                    }

                    return _ret;
                }

                valid_t RenderTileOES () {
                    valid_t _ret = glGetError () == GL_NO_ERROR;

                    constexpr char const _vtx_src [] =
                        "#version 100                              \n"
                        "attribute vec3 position;                  \n"
                        "varying vec2 coordinates;                 \n"
                        "void main () {                            \n"
                            "gl_Position = vec4 (position.xyz, 1); \n"
                            "coordinates = position.xy;            \n"
                        "}                                         \n"
                        ;

                    constexpr char  const _frag_src [] =
                        "#version 100                                                             \n"
                        "#extension GL_OES_EGL_image_external : require                           \n"
                        "precision mediump float;                                                 \n"
                        "uniform samplerExternalOES sampler;                                      \n"
                        "uniform float opacity;                                                   \n"
                        "varying vec2 coordinates;                                                \n"
                        "void main () {                                                           \n"
                            "gl_FragColor = vec4 (texture2D (sampler, coordinates).rgb, opacity); \n"
                        "}                                                                        \n"
                        ;

                    static_assert (std::is_same <GLfloat, GLES::offset::coordinate_t>:: value != false);
                    std::array <GLfloat, 4 * VerticeDimensions> const _vert = {
                        0.0f, 0.0f, 0.0f /* v0 */,
                        1.0f, 0.0f, 0.0f /* v1 */,
                        0.0f, 1.0f, 0.0f /* v2 */,
                        1.0f, 1.0f, 0.0f /* v3 */
                    };

                    if (_ret != false) {
                         glDisable (GL_BLEND);
                        _ret = glGetError () == GL_NO_ERROR;
                    }

                    _ret = _ret
                            && RenderColor (false, false, false)
                            && SetupProgram (_vtx_src, _frag_src)
                            && RenderPolygon (_vert);

                    return _ret;
                }

                valid_t RenderTile () {
                    valid_t _ret = glGetError () == GL_NO_ERROR;

                    constexpr char const _vtx_src [] =
                        "#version 100                              \n"
                        "attribute vec3 position;                  \n"
                        "varying vec2 coordinates;                 \n"
                        "void main () {                            \n"
                            "gl_Position = vec4 (position.xyz, 1); \n"
                            "coordinates = position.xy;            \n"
                        "}                                         \n"
                        ;

                    constexpr char  const _frag_src [] =
                        "#version 100                                                             \n"
                        "precision mediump float;                                                 \n"
                        "uniform sampler2D sampler;                                               \n"
                        // Required by RenderPolygon
                        "uniform float opacity;                                                   \n"
                        "varying vec2 coordinates;                                                \n"
                        "void main () {                                                           \n"
                            "gl_FragColor = vec4 (texture2D (sampler, coordinates).rgba);         \n"
                        "}                                                                        \n"
                        ;

                    static_assert (std::is_same <GLfloat, GLES::offset::coordinate_t>:: value != false);
                    std::array <GLfloat, 4 * VerticeDimensions> const _vert = {
                        0.0f, 0.0f, _offset._z /* v0 */,
                        1.0f, 0.0f, _offset._z /* v1 */,
                        0.0f, 1.0f, _offset._z /* v2 */,
                        1.0f, 1.0f, _offset._z /* v3 */
                    };


// TODO: version match
#ifdef _0
                    using string_t = std::string::value_type;
                    string_t const * _ext = reinterpret_cast <string_t const *> ( glGetString (GL_SHADING_LANGUAGE_VERSION) );
#endif


                    _ret = glGetError () == GL_NO_ERROR
                            && SetupProgram (_vtx_src, _frag_src)
                            && RenderPolygon (_vert);

                    return _ret;
                }


                template <size_t N>
                valid_t RenderPolygon (std::array <GLfloat, N> const & vert) {
                    valid_t _ret = glGetError () == GL_NO_ERROR;

                    if (_ret != false) {
                        GLuint _prog = 0;

                        if (_ret != false) {
                            glGetIntegerv (GL_CURRENT_PROGRAM, reinterpret_cast <GLint *> (&_prog));
                            _ret = glGetError () == GL_NO_ERROR;
                        }

                        GLint _loc_vert = 0, _loc_op = 0;

                        if (_ret != false) {
                            _loc_op = glGetUniformLocation (_prog, "opacity");
                            _ret = glGetError () == GL_NO_ERROR;
                        }

                        if (_ret != false) {
                            glUniform1f (_loc_op, _opacity._alpha);
                            _ret = glGetError () == GL_NO_ERROR;
                        }

                        if (_ret != false) {
                            _loc_vert = glGetAttribLocation (_prog, "position");
                            _ret = glGetError () == GL_NO_ERROR;
                        }

                        if (_ret != false) {
                            glVertexAttribPointer (_loc_vert, VerticeDimensions, GL_FLOAT, GL_FALSE, 0, vert.data ());
                            _ret = glGetError () == GL_NO_ERROR;
                        }

                        if (_ret != false) {
                            glEnableVertexAttribArray (_loc_vert);
                            _ret = glGetError () == GL_NO_ERROR;
                        }

                        if (_ret != false) {
                            glDrawArrays (GL_TRIANGLE_STRIP, 0, vert.size () / VerticeDimensions);
                            _ret = glGetError () == GL_NO_ERROR;
                        }

                        if (_ret != false) {
                            glDisableVertexAttribArray (_loc_vert);
                            _ret = glGetError () == GL_NO_ERROR;
                        }
                    }

                    return _ret;
                }

                valid_t Supported (std::string const & name) {
                    valid_t _ret = false;

                    using string_t = std::string::value_type;
                    using ustring_t = std::make_unsigned < string_t > :: type;

                    // Identical underlying types except for signedness
                    static_assert (std::is_same < ustring_t, GLubyte > :: value != false);

                    string_t const * _ext = reinterpret_cast <string_t const *> ( glGetString (GL_EXTENSIONS) );

                    _ret = _ext != nullptr
                           && name.size () > 0
                           && ( std::string (_ext).find (name)
                                != std::string::npos );

                    return _ret;
                }

// TODO: parameter names
                valid_t SetupViewport (EGL::width_t _width, EGL::height_t _height) {
                    valid_t _ret = glGetError () == GL_NO_ERROR;

                    GLint _dims [2] = {0, 0};

                    if (_ret != false) {
                        glGetIntegerv (GL_MAX_VIEWPORT_DIMS, &_dims [0]);
                        _ret = glGetError () == GL_NO_ERROR;
                    }

                    if (_ret != false) {
#define _QUIRKS
#ifdef _QUIRKS
                        // glViewport (x, y, width, height)
                        //
                        // Applied width = width / 2
                        // Applied height = height / 2
                        // Applied origin's x = width / 2 + x
                        // Applied origin's y = height / 2 + y
                        //
                        // Compensate to origin bottom left and true size by
                        // glViewport (-width, -height, width * 2, height * 2)
                        //
                        // _offset is in the range -1..1 wrt to origin, so the effective value maps to -width to width, -height to height

                        constexpr uint8_t _mult = 2;

                        using common_t = std::common_type < decltype (_width), decltype (_height), decltype (_mult), decltype (_scale._horiz), decltype (_scale._vert), decltype (_offset._x), decltype (_offset._y), remove_pointer < std::decay < decltype (_dims) > :: type > :: type > :: type;

                        common_t _quirk_width = static_cast <common_t> (_width) * static_cast <common_t> (_mult) * static_cast <common_t> (_scale._horiz);
                        common_t _quirk_height = static_cast <common_t> (_height) * static_cast <common_t> (_mult) * static_cast <common_t> (_scale._vert);

                        common_t _quirk_x = ( static_cast <common_t> (-_width) * static_cast <common_t> (_scale._horiz) ) + ( static_cast <common_t> (_offset._x) * static_cast <common_t> (_width) );
                        common_t _quirk_y = ( static_cast <common_t> (-_height) * static_cast <common_t> (_scale._vert) ) + ( static_cast <common_t> (_offset._y) * static_cast <common_t> (_height) );

                        if (   _quirk_x < ( -_quirk_width / static_cast <common_t> (_mult) )
                            || _quirk_y < ( -_quirk_height / static_cast <common_t> (_mult) )
                            || _quirk_x  > static_cast <common_t> (0)
                            || _quirk_y  > static_cast <common_t> (0)
                            || _quirk_width > ( static_cast <common_t> (_width) * static_cast <common_t> (_mult) )
                            || _quirk_height > ( static_cast <common_t> (_height) * static_cast <common_t> (_mult) )
                            || static_cast <common_t> (_width) > static_cast <common_t> (_dims [0])
                            || static_cast <common_t> (_height) > static_cast <common_t> (_dims [1])
                        ) {
                            // Clipping, or undefined / unknown behavior
                            std::cout << "Warning: possible clipping or unknown behavior detected. [" << _quirk_x << ", " << _quirk_y << ", " << _quirk_width << ", " << _quirk_height << ", " << _width << ", " << _height << ", " << _dims [0] << ", " << _dims [1] << "]" << std::endl;
                        }

                        glViewport (static_cast <GLint> (_quirk_x), static_cast <GLint> (_quirk_y), static_cast <GLsizei> (_quirk_width), static_cast <GLsizei> (_quirk_height));
#else
                        glViewport (0, 0, _width, _height);
#endif


                }

                return _ret;
            }

       };

//                static_assert ( (std::is_convertible < decltype (_dpy->Native ()), EGLNativeDisplayType > :: value) != false);
//                static_assert ( (std::is_convertible < decltype (_surf->Native ()), EGLNativeWindowType > :: value) != false);

       class EGL  {

#define XSTRINGIFY(X) STRINGIFY(X)
#define STRINGIFY(X) #X

#ifdef EGL_VERSION_1_5
#define KHRFIX(name) name
#define _EGL_SYNC_FENCE EGL_SYNC_FENCE
#define _EGL_NO_SYNC EGL_NO_SYNC
#define _EGL_FOREVER EGL_FOREVER
#define _EGL_NO_IMAGE EGL_NO_IMAGE
#define _EGL_NATIVE_PIXMAP EGL_NATIVE_PIXMAP_KHR
#define _EGL_CONDITION_SATISFIED EGL_CONDITION_SATISFIED
#define _EGL_SYNC_STATUS EGL_SYNC_STATUS
#define _EGL_SIGNALED EGL_SIGNALED
#define _EGL_SYNC_FLUSH_COMMANDS_BIT EGL_SYNC_FLUSH_COMMANDS_BIT
#else
#define _KHRFIX(left, right) left ## right
#define KHRFIX(name) _KHRFIX(name, KHR)
#define _EGL_SYNC_FENCE EGL_SYNC_FENCE_KHR
#define _EGL_NO_SYNC EGL_NO_SYNC_KHR
#define _EGL_FOREVER EGL_FOREVER_KHR
#define _EGL_NO_IMAGE EGL_NO_IMAGE_KHR
#define _EGL_NATIVE_PIXMAP EGL_NATIVE_PIXMAP_KHR
#define _EGL_CONDITION_SATISFIED EGL_CONDITION_SATISFIED_KHR
#define _EGL_SYNC_STATUS EGL_SYNC_STATUS_KHR
#define _EGL_SIGNALED EGL_SIGNALED_KHR
#define _EGL_SYNC_FLUSH_COMMANDS_BIT EGL_SYNC_FLUSH_COMMANDS_BIT_KHR

                using EGLAttrib = EGLint;
#endif
                using EGLuint64KHR = khronos_uint64_t;

            public :

                class Sync final {

                    public :

                        using dpy_t = EGLDisplay;
                        using sync_t = KHRFIX (EGLSync);

                    private :

                        sync_t _sync;
                        dpy_t & _dpy;

                    public :
                        Sync () = delete;

                        explicit Sync (dpy_t & dpy) : _dpy {dpy} {
#ifdef _V3D_FENCE
                            static bool _supported = EGL::Supported (dpy, "EGL_KHR_fence_sync") != false;

                            assert (dpy != InvalidDpy ());

                            _sync = ( _supported && dpy != InvalidDpy () ) != false ? KHRFIX (eglCreateSync) (dpy, _EGL_SYNC_FENCE, nullptr) : InvalidSync ();
#else
                            _sync = InvalidSync ();
#endif
                        }

                        ~Sync () {
                            if (_sync == InvalidSync ()) {
                                // Error creating sync object or unable to create one
                                glFinish ();
                            }
                            else {
                                // Mandatory
                                glFlush ();

                                // .. but still execute, when needed, an additional flush to be on the safe sidei, and avoid a dreaded  deadlock
                                EGLint _val = static_cast <EGLint> ( KHRFIX (eglClientWaitSync) (_dpy, _sync, _EGL_SYNC_FLUSH_COMMANDS_BIT, _EGL_FOREVER) );

                                if (_val == static_cast <EGLint> (EGL_FALSE) || _val != static_cast <EGLint> (_EGL_CONDITION_SATISFIED)) {
                                    EGLAttrib _status;

                                    bool _ret = KHRFIX (eglGetSyncAttrib) (_dpy, _sync, _EGL_SYNC_STATUS, &_status) != EGL_FALSE;

                                    _ret = _ret && _status == _EGL_SIGNALED;

                                    // Assert on error
                                    if (_ret != true) {
                                        TRACE (Trace::Error, (_T ("EGL: synchronization primitive")) );
                                        assert (false);
                                    }
                                }

                                // Consume the (possible) error(s)
                                /* ELGint */ glGetError ();
                                /* ELGint */ eglGetError ();
                            }
                        }

                        static constexpr dpy_t InvalidDpy () { return EGL_NO_DISPLAY; }
                        static constexpr sync_t InvalidSync () { return _EGL_NO_SYNC; }

                    private :

                        void * operator new (size_t) = delete;
                        void * operator new [] (size_t) = delete;
                        void operator delete (void *) = delete;
                        void operator delete [] (void *) = delete;

                };

            private :

                // Define the 'invalid' value
                static_assert (std::is_pointer <EGLConfig>::value != false);
                static constexpr void * const EGL_NO_CONFIG = nullptr;

                Sync::dpy_t _dpy = Sync::InvalidDpy ();
                EGLConfig _conf = EGL_NO_CONFIG;
                EGLContext _ctx = EGL_NO_CONTEXT;
                EGLSurface _surf = EGL_NO_SURFACE;

                WPEFramework::Plugin::EGL::width_t  _width = 0;
                WPEFramework::Plugin::EGL::height_t _height = 0;

                Natives const & _natives;

                bool _valid = false;

            public :

                using dpy_t = decltype (_dpy);
                using surf_t = decltype (_surf);
                using ctx_t = decltype (_ctx);
                using height_t = decltype (_height);
                using width_t = decltype (_width);
                using valid_t = decltype (_valid);

                using size_t = EGLint;

                using img_t = WPEFramework::Plugin::EGL::img_t;

                EGL () = delete;
                EGL (Natives const & natives) : _natives { natives }, _valid { Initialize () } {}
                ~EGL () {
                    _valid = false;
                    DeInitialize ();
                }

                static constexpr img_t InvalidImage () { return WPEFramework::Plugin::EGL::InvalidImage (); }
                static constexpr dpy_t InvalidDpy () { return Sync::InvalidDpy (); }
                static constexpr ctx_t InvalidCtx () { return EGL_NO_CONTEXT; }
                static constexpr surf_t InvalidSurf () { return EGL_NO_SURFACE; }

                static constexpr EGL::size_t RedBufferSize () { return static_cast <EGL::size_t> (8); }
                static constexpr EGL::size_t GreenBufferSize () { return static_cast <EGL::size_t> (8); }
                static constexpr EGL::size_t BlueBufferSize () { return static_cast <EGL::size_t> (8); }
                static constexpr EGL::size_t AlphaBufferSize () { return static_cast <EGL::size_t> (8); }
                // For OpenGL ES 2.0 the only possible value is 16
                static constexpr EGL::size_t DepthBufferSize () { return static_cast <EGL::size_t> (GLES::MajorVersion () == static_cast <GLES::version_t> (2) ? 16 : 0); }

                dpy_t Display () const { return _dpy; }
                surf_t Surface () const { return _surf; }

                height_t Height () const { return _height; }
                width_t Width () const { return _width; }

                static img_t CreateImage (EGL const & egl, ClientSurface::surf_t const & surf) {
                    img_t _ret = InvalidImage ();

                    if (egl.Valid () && ( Supported (egl.Display (), "EGL_KHR_image") && Supported (egl.Display (), "EGL_KHR_image_base") && Supported (egl.Display (), "EGL_EXT_image_dma_buf_import") && Supported (egl.Display (), "EGL_EXT_image_dma_buf_import_modifiers") ) != false ) {
                        static_assert ((std::is_same <dpy_t, EGLDisplay> :: value && std::is_same <ctx_t, EGLContext> :: value && std::is_same <img_t, KHRFIX (EGLImage)> :: value ) != false);

                        constexpr char _methodName [] = XSTRINGIFY ( KHRFIX (eglCreateImage) );

                        static KHRFIX (EGLImage) (* _eglCreateImage) (EGLDisplay, EGLContext, EGLenum, EGLClientBuffer, EGLAttrib const *) = reinterpret_cast < KHRFIX (EGLImage) (*) (EGLDisplay, EGLContext, EGLenum, EGLClientBuffer, EGLAttrib const * ) > (eglGetProcAddress ( _methodName ));

                        if (_eglCreateImage != nullptr) {

                            auto _width = gbm_bo_get_width (surf._buf);
                            auto _height = gbm_bo_get_height (surf._buf);
                            auto _stride = gbm_bo_get_stride (surf._buf);
                            auto _format = gbm_bo_get_format (surf._buf);
                            auto _modifier = gbm_bo_get_modifier (surf._buf);

                            using width_t =  decltype (_width);
                            using height_t = decltype (_height);
                            using stride_t = decltype (_stride);
                            using format_t = decltype (_format);
                            using fd_t = decltype (surf._fd);
                            using modifier_t = decltype (_modifier);

                            // Does it already exist ?
                            assert (surf._fd > -1);

                            // Test our initial assumption
                            assert (_format == ModeSet::SupportedBufferType ());
                            assert (_modifier == ModeSet::FormatModifier ());

                            // Narrowing detection

                            // Enable narrowing detecttion
// TODO:
                            constexpr bool _enable = false;

                            // (Almost) all will fail!
                            if (_narrowing < width_t, EGLAttrib, _enable > :: value != false
                                && _narrowing < height_t, EGLAttrib, _enable > :: value != false
                                && _narrowing < stride_t, EGLAttrib, _enable > :: value != false
                                && _narrowing < format_t, EGLAttrib, _enable > :: value != false
                                && _narrowing < fd_t, EGLAttrib, _enable > :: value != false
                                && _narrowing < modifier_t, EGLuint64KHR, true> :: value) {
                                TRACE_WITHOUT_THIS (Trace::Information, (_T ("Possible narrowing detected!")));
                            }

                            // EGL may report differently than DRM
                            ModeSet::GBM::dev_t _dev = gbm_bo_get_device (surf._buf);
                            ModeSet::GBM::fd_t _fd = gbm_device_get_fd (_dev);

                            // EGL may report differently than DRM
                            auto _list_d_for = ModeSet::AvailableFormats (static_cast <ModeSet::DRM::fd_t> (_fd));
                            auto _it_d_for = std::find (_list_d_for.begin (), _list_d_for.end (), _format);

                            bool _valid = _it_d_for != _list_d_for.end ();

                            // Query EGL
                            static EGLBoolean (* _eglQueryDmaBufFormatsEXT) (EGLDisplay, EGLint, EGLint *, EGLint *) = reinterpret_cast < EGLBoolean (*) (EGLDisplay, EGLint, EGLint *, EGLint *) > (eglGetProcAddress ("eglQueryDmaBufFormatsEXT"));
                            static EGLBoolean (* _eglQueryDmaBufModifiersEXT) (EGLDisplay,EGLint, EGLint, EGLuint64KHR *, EGLBoolean *, EGLint *) = reinterpret_cast < EGLBoolean (*) (EGLDisplay,EGLint, EGLint, EGLuint64KHR *, EGLBoolean *, EGLint *) > (eglGetProcAddress ("eglQueryDmaBufModifiersEXT"));

                            _valid = _valid && _eglQueryDmaBufFormatsEXT != nullptr && _eglQueryDmaBufModifiersEXT != nullptr;

                            EGLint _count = 0;

                            _valid = _valid && _eglQueryDmaBufFormatsEXT (egl.Display (), 0, nullptr, &_count) != EGL_FALSE;

                            _valid = _valid && _eglQueryDmaBufFormatsEXT (egl.Display (), 0, nullptr, &_count) != EGL_FALSE;

                            EGLint _formats [_count];

                            _valid = _valid && _eglQueryDmaBufFormatsEXT (egl.Display (), _count, &_formats [0], &_count) != EGL_FALSE;

                            // _format should be listed as supported
                            if (_valid != false) {
                                std::list <EGLint> _list_e_for (&_formats [0], &_formats [_count]);

                                auto _it_e_for = std::find (_list_e_for.begin (), _list_e_for.end (), _format);

                                _valid = _it_e_for != _list_e_for.end ();
                            }

                            _valid = _valid && _eglQueryDmaBufModifiersEXT (egl.Display (), _format, 0, nullptr, nullptr, &_count) != EGL_FALSE;

                            EGLuint64KHR _modifiers [_count];
                            EGLBoolean _external [_count];

                            // External is required to exclusive use withGL_TEXTURE_EXTERNAL_OES
                            _valid = _valid && _eglQueryDmaBufModifiersEXT (egl.Display (), _format, _count, &_modifiers [0], &_external [0], &_count) != FALSE;

                            // _modifier should be listed as supported, and _external should be true
                            if (_valid != false) {
                                std::list <EGLuint64KHR> _list_e_mod (&_modifiers [0], &_modifiers [_count]);

                                auto _it_e_mod = std::find (_list_e_mod.begin (), _list_e_mod.end (), static_cast <EGLuint64KHR> (_modifier));

                                _valid = _it_e_mod != _list_e_mod.end ();
                            }

                            if (_valid != false) {
                                EGLAttrib const _attrs [] = {
                                    EGL_WIDTH, static_cast <EGLAttrib> (_width),
                                    EGL_HEIGHT, static_cast <EGLAttrib> (_height),
                                    EGL_LINUX_DRM_FOURCC_EXT, static_cast <EGLAttrib> (_format),
                                    EGL_DMA_BUF_PLANE0_FD_EXT, static_cast <EGLAttrib> (surf._fd),
// TODO: magic constant
                                    EGL_DMA_BUF_PLANE0_OFFSET_EXT, 0,
                                    EGL_DMA_BUF_PLANE0_PITCH_EXT, static_cast <EGLAttrib> (_stride),
                                    EGL_DMA_BUF_PLANE0_MODIFIER_LO_EXT, static_cast <EGLAttrib> (static_cast <EGLuint64KHR> (_modifier) & 0xFFFFFFFF),
                                    EGL_DMA_BUF_PLANE0_MODIFIER_HI_EXT, static_cast <EGLAttrib> (static_cast <EGLuint64KHR> (_modifier) >> 32),
                                    EGL_IMAGE_PRESERVED_KHR, EGL_TRUE,
                                    EGL_NONE
                                };

                                static_assert (std::is_convertible < decltype (surf._buf), EGLClientBuffer > :: value != false);
                                _ret = _eglCreateImage (egl.Display (), EGL_NO_CONTEXT, EGL_LINUX_DMA_BUF_EXT, nullptr, _attrs);
                            }
                        }
                        else {
                            // Error
                            TRACE_WITHOUT_THIS (Trace::Error, _T ("%s is unavailable or invalid parameters."), _methodName);
                        }
                    }
                    else {
                        TRACE_WITHOUT_THIS (Trace::Error, _T ("EGL is not properly initialized."));
                    }

                    return _ret;
                }

                static img_t DestroyImage (EGL const & egl, ClientSurface::surf_t const & surf) {
                    img_t _ret = surf._khr;

                    if (egl.Valid () && Supported (egl.Display (), "EGL_KHR_image") && Supported (egl.Display (), "EGL_KHR_image_base") != false ) {

                        static_assert ((std::is_same <dpy_t, EGLDisplay> :: value && std::is_same <ctx_t, EGLContext> :: value && std::is_same <img_t, KHRFIX (EGLImage)> :: value ) != false);

                        constexpr char _methodName [] = XSTRINGIFY ( KHRFIX (eglDestroyImage) );

                        static EGLBoolean (* _eglDestroyImage) (EGLDisplay, KHRFIX (EGLImage)) = reinterpret_cast < EGLBoolean (*) (EGLDisplay, KHRFIX (EGLImage)) > (eglGetProcAddress ( KHRFIX ("eglDestroyImage") ));

                        if (_eglDestroyImage != nullptr && surf.RenderComplete () != false) {
// TODO: Leak?
                            _ret = _eglDestroyImage (egl.Display (), surf._khr) != EGL_FALSE ? EGL::InvalidImage () : _ret;
                        }
                        else {
                            // Error
                            TRACE_WITHOUT_THIS (Trace::Error, _T ("%s is unavailable or invalid parameters are provided."), _methodName);
                        }
                    }
                    else {
                        TRACE_WITHOUT_THIS (Trace::Error, _T ("EGL is not properly initialized."));
                    }

                    return _ret;
                }

                valid_t Valid () const { return _valid; }

            private :

                valid_t Initialize () {
                    valid_t _ret = _natives.Valid ();

                    if (_ret != false) {

                        if (_dpy != EGL_NO_DISPLAY) {
                            _ret = false;

                            if (eglTerminate (_dpy) != EGL_FALSE) {
                                _ret = true;
                            }
                        }
                    }

                    if (_ret != false) {
                        using native_dpy_no_const =  _remove_const < Natives::dpy_t > :: type;

                        // Again. EGL does use const sparsely
                        _dpy = eglGetDisplay (const_cast < native_dpy_no_const > (_natives.Display ()) );
                        _ret = _dpy != EGL_NO_DISPLAY;
                    }

                    if (_ret != false) {
                        EGLint _major = 0, _minor = 0;
                        _ret = eglInitialize (_dpy, &_major, &_minor) != EGL_FALSE;

                        // Just take the easy approach (for now)
                        static_assert ((std::is_integral <decltype (_major)> :: value) != false);
                        static_assert ((std::is_integral <decltype (_minor)> :: value) != false);
                        TRACE (Trace::Information, (_T ("EGL version : %d.%d"), static_cast <int> (_major), static_cast <int> (_minor)));
                    }

                    if (_ret != false) {
                        static_assert (GLES::MajorVersion () == static_cast < GLES::version_t > (2));

                        constexpr EGLint const _attr [] = {
                            EGL_SURFACE_TYPE, EGL_WINDOW_BIT,
                            EGL_RED_SIZE    , RedBufferSize (),
                            EGL_GREEN_SIZE  , GreenBufferSize (),
                            EGL_BLUE_SIZE   , BlueBufferSize (),
                            EGL_ALPHA_SIZE  , AlphaBufferSize (),
                            EGL_BUFFER_SIZE , RedBufferSize () + GreenBufferSize () + BlueBufferSize () + AlphaBufferSize (),
                            EGL_RENDERABLE_TYPE, EGL_OPENGL_ES2_BIT,
                            EGL_DEPTH_SIZE  , DepthBufferSize (),
                            EGL_NONE
                        };

                        EGLint _count = 0;

                        if (eglGetConfigs (_dpy, nullptr, 0, &_count) != EGL_TRUE) {
                            _count = 1;
                        }

                        std::vector <EGLConfig> _confs (_count, EGL_NO_CONFIG);

                        /* EGLBoolean */ eglChooseConfig (_dpy, &_attr [0], _confs.data (), _confs.size (), &_count);

                        _conf = _confs [0];

                        _ret = _conf != EGL_NO_CONFIG;
                    }


                    if (_ret != false) {
                        EGLenum _api = eglQueryAPI ();

                        _ret = _api == EGL_OPENGL_ES_API;

                        if (_ret != true) {
                            /* EGLBoolean */ eglBindAPI (EGL_OPENGL_ES_API);
                            _ret = eglGetError () == EGL_SUCCESS;
                        }
                    }


                    if (_ret != false) {
                        static_assert (_narrowing <GLES::version_t, EGLint, true> :: value != false);

                        constexpr EGLint const _attr [] = {
                            EGL_CONTEXT_CLIENT_VERSION, static_cast <GLES::version_t> (GLES::MajorVersion ()),
                            EGL_NONE
                        };

                        _ctx = eglCreateContext (_dpy, _conf, EGL_NO_CONTEXT, _attr);
                        _ret = _ctx != EGL_NO_CONTEXT;
                    }

                    if (_ret != false) {
                        constexpr EGLint const _attr [] = {
                            EGL_NONE
                        };

                        _surf = eglCreateWindowSurface (_dpy, _conf, _natives.Surface (), &_attr [0]);
                        _ret = _surf != EGL_NO_SURFACE;
                    }

                    if (_ret != true) {
                        DeInitialize ();
                    }

                    return _ret;
                }

                void DeInitialize () {
                    _valid = false;
                    /* EGLBoolean */ eglTerminate (_dpy);
                }

            public :

                // Although compile / build time may succeed, runtime checks are also mandatory
                static bool Supported (dpy_t const dpy, std::string const & name) {
                    bool _ret = false;

                    static_assert ((std::is_same <dpy_t, EGLDisplay> :: value) != false);

#ifdef EGL_VERSION_1_5
                    // KHR extentions that have become part of the standard

                    // Sync capability
                    _ret = name.find ("EGL_KHR_fence_sync") != std::string::npos
                           /* CreateImage / DestroyImage */
                           || name.find ("EGL_KHR_image") != std::string::npos
                           || name.find ("EGL_KHR_image_base") != std::string::npos;
#endif

                    if (_ret != true) {
                        static_assert (std::is_same <std::string::value_type, char> :: value != false);
                        char const * _ext = eglQueryString (dpy, EGL_EXTENSIONS);

                        _ret =  _ext != nullptr
                                && name.size () > 0
                                && ( std::string (_ext).find (name)
                                     != std::string::npos );
                    }

                    return _ret;
                }

                bool Render (GLES & gles) {
                    // Ensure the client API is set per thread basis
                    bool _ret = Valid () != false && eglMakeCurrent(_dpy, _surf, _surf, _ctx) != EGL_FALSE && eglBindAPI (EGL_OPENGL_ES_API) != EGL_FALSE;

                    if (_ret != false) {
                        _ret = eglSwapBuffers (_dpy, _surf) != EGL_FALSE;

                        // Guarantuee all (previous) effects of client API and frame buffer state are realized
                        { WPEFramework::Plugin::CompositorImplementation::EGL::Sync _sync (_dpy); }

                        // Avoid any memory leak if the local thread is stopped (by another thread)
                        _ret = eglMakeCurrent (_dpy, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT) != EGL_FALSE && _ret;
                    }

                    if (_ret != true) {
                        TRACE (Trace::Error, (_T ("Failed to complete rendering content.")));
                    }

                    return _ret;
                }

                bool Render (std::function < GLES::valid_t () > func, bool post) {
                    // Ensure the client API is set per thread basis
                    bool _ret = Valid () != false && eglMakeCurrent(_dpy, _surf, _surf, _ctx) != EGL_FALSE && eglBindAPI (EGL_OPENGL_ES_API) != EGL_FALSE;

                    if (_ret != false) {
                        if (post != false) {
                            _ret = func () != false;

                            { WPEFramework::Plugin::CompositorImplementation::EGL::Sync _sync (_dpy); }

                           _ret = _ret && eglSwapBuffers (_dpy, _surf) != EGL_FALSE;
                        }
                        else {
                            _ret = eglSwapBuffers (_dpy, _surf) != EGL_FALSE && func () != false;
                        }

                        // Guarantuee all (previous) effects of client API and frame buffer state are realized
                        { WPEFramework::Plugin::CompositorImplementation::EGL::Sync _sync (_dpy); }

                        // Expensive, but it avoids any memory leak if the local thread is stopped (by another thread)
                        _ret = eglMakeCurrent (_dpy, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT) != EGL_FALSE && _ret;
                    }

                    if (_ret != true) {
                        TRACE (Trace::Error, (_T ("Failed to complete rendering content.")));
                    }

                    return _ret;
                }

                bool Render (std::function < GLES::valid_t () > prefunc, std::function < GLES::valid_t () > postfunc) {
                    // Ensure the client API is set per thread basis
                    bool _ret = Valid () != false && eglMakeCurrent(_dpy, _surf, _surf, _ctx) != EGL_FALSE && eglBindAPI (EGL_OPENGL_ES_API) != EGL_FALSE;

                    if (_ret != false) {
                        _ret = prefunc () != false;

                        // Guarantuee all (previous) effects of client API and frame buffer state are realized
                        { WPEFramework::Plugin::CompositorImplementation::EGL::Sync _sync (_dpy); }

                        _ret = _ret && eglSwapBuffers (_dpy, _surf) != EGL_FALSE && postfunc () != false;

                        // Guarantuee all (previous) effects of client API and frame buffer state are realized
                        { WPEFramework::Plugin::CompositorImplementation::EGL::Sync _sync (_dpy); }

                        // Expensive, but avoids any memory leak if the local thread is stopped (by another thread)
                        _ret = eglMakeCurrent (_dpy, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT) != EGL_FALSE && _ret;
                    }

                    if (_ret != true) {
                        TRACE (Trace::Error, (_T ("Failed to complete rendering content.")));
                    }

                    return _ret;
                }

                bool RenderWithoutSwap (std::function < GLES::valid_t () > func) {
                    // Ensure the client API is set per thread basis
                    bool _ret = Valid () != false && eglMakeCurrent(_dpy, _surf, _surf, _ctx) != EGL_FALSE && eglBindAPI (EGL_OPENGL_ES_API) != EGL_FALSE;

                    if (_ret != false) {
                        _ret = func () != false;

                        // Guarantuee all (previous) effects of client API and frame buffer state are realized
                        { WPEFramework::Plugin::CompositorImplementation::EGL::Sync _sync (_dpy); }

                        // Expensive, but avoids any memory leak if the local thread is stopped (by another thread)
                        _ret = eglMakeCurrent (_dpy, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT) != EGL_FALSE && _ret;
                    }

                    if (_ret != true) {
                        TRACE (Trace::Error, (_T ("Failed to complete rendering content.")));
                    }

                    return _ret;
                }

#undef STRINGIFY
#ifdef _KHRFIX
#undef _KHRFIX
#endif
#undef KHRFIX
#undef _EGL_SYNC_FENCE
#undef _EGL_NO_SYNC
#undef _EGL_FOREVER
#undef _EGL_NO_IMAGE
#undef _EGL_NATIVE_PIXMAP
#undef _EGL_CONDITION_SATISFIED
#undef _EGL_SYNC_STATUS
#undef _EGL_SIGNALED
#undef _EGL_SYNC_FLUSH_COMMANDS_BIT
        };

    public:
        CompositorImplementation()
            : _adminLock()
            , _service(nullptr)
            , _engine()
            , _externalAccess(nullptr)
            , _observers()
            , _clients()
            , _platform()
            , _dma { nullptr }
            , _natives (_platform)
            , _egl (_natives)
        {
        }
        ~CompositorImplementation()
        {
            if (_dma != nullptr) {
                delete _dma;
            }

            _dma = nullptr;

#ifdef _0 // Only if destructors are not called
            _clients.Visit (
                [this] (string const & name, const Core::ProxyType<ClientSurface>& client) {
                    if ( (client.IsValid () && _egl.Valid () ) != false) {
                        /* surf_t */ client->Surface ( EGL::DestroyImage ( _egl, client->Surface () ));
                    }
                }
            );
#else
            _clients.Clear();
#endif

            if (_externalAccess != nullptr) {
                delete _externalAccess;
                _engine.Release();
            }
        }

        BEGIN_INTERFACE_MAP(CompositorImplementation)
        INTERFACE_ENTRY(Exchange::IComposition)
        INTERFACE_ENTRY(Exchange::IComposition::IDisplay)
        END_INTERFACE_MAP

    private:
        class Config : public Core::JSON::Container {
        private:
            Config(const Config&) = delete;
            Config& operator=(const Config&) = delete;

        public:
            Config()
                : Core::JSON::Container()
                , Connector(_T("/tmp/compositor"))
                , Port(_T("HDMI0"))
            {
                Add(_T("connector"), &Connector);
                Add(_T("port"), &Port);
            }

            ~Config()
            {
            }

        public:
            Core::JSON::String Connector;
            Core::JSON::String Port;
        };

    public:

        bool FDFor (std::string const & name, int & fd) {

            std::string _prop;

            bool _ret = FDFor (name, fd, _prop);

            return _ret;
        }

        bool FDFor (std::string const & name, int & fd, std::string & properties) {
            std::lock_guard < decltype (_clientLock) > const lock (_clientLock);

            bool _ret = false;

            TRACE (Trace::Information, (_T ("%s requested a DMA transfer."), name.c_str ()));

            /* ProxyType <> */ auto _client = _clients.Find (name);

            if (_client.IsValid () != true ||  name.compare (_client->Name ()) != 0) {
                TRACE (Trace::Error, (_T ("%s does not appear to be a valid client."), name.length () > 0 ? name.c_str () : "'<no name provided>'"));
            }
            else {
                if (_dma != nullptr) {
                    using fd_t = std::remove_reference < decltype (fd) > :: type;
                    using class_t = decltype (_client);
                    using return_t = decltype ( std::declval < class_t > ().operator-> ()->Native () );

                    static_assert ((std::is_convertible < return_t, fd_t > :: value) != false);
                    // Likely to almost always fail
//                    static_assert ((sizeof ( return_t ) <= sizeof ( fd_t )) != false);

                    fd = static_cast < fd_t > ( _client->Native () );

                    _ret = fd > -1;
                }
            }

            ClientSurface::surf_t _surf;

            if (_ret != false) {
                _surf = _client->Surface ( EGL::CreateImage (_egl, _client->Surface () ));

                _ret = (_surf.Valid () != false);
            }

            if (_ret != false) {
                    properties.clear ();

                    auto _width = gbm_bo_get_width (_surf._buf);
                    auto _height = gbm_bo_get_height (_surf._buf);
                    auto _stride = gbm_bo_get_stride (_surf._buf);
                    auto _format = gbm_bo_get_format (_surf._buf);
                    auto _modifier = gbm_bo_get_modifier (_surf._buf);

                    constexpr char _spacer [] = ":";

                    properties = std::string (_spacer)
                                + std::to_string (_width).append (_spacer)
                                + std::to_string (_height).append (_spacer)
                                + std::to_string (_stride).append (_spacer)
                                + std::to_string (_format).append (_spacer)
                                + std::to_string (_modifier);
            }

            return _ret;
        }

        // The (remote) caller should not continue to render to any shared resource until this completes
        bool CompositeFor (std::string const & name) {
            std::lock_guard < decltype (_clientLock) > const lock (_clientLock);

            bool _ret = false;

            /* ProxyType <> */ auto _client = _clients.Find (name);

            if (_client.IsValid () != true ||  name.compare (_client->Name ()) != 0) {
                TRACE (Trace::Error, (_T ("%s does not appear to be a valid client."), name.c_str ()));
            }
            else {
                ClientSurface::surf_t const & _surf = _client->Surface ();

                _ret = ( _surf.RenderComplete () && _egl.Valid () && _gles.Valid () ) != false;

                if (_ret != false) {
                    TRACE (Trace::Information, (_T ("Client has an associated EGL image.")));

                    auto _width = gbm_bo_get_width (_surf._buf);
                    auto _height = gbm_bo_get_height (_surf._buf);

                    using width_t = decltype (_width);
                    using height_t = decltype (_height);

                    // Narrowing detection

                    // Enable narrowing detection
// TODO:
                    constexpr bool _enable = false;

                    // (Almost) all will fail!
                    if (_narrowing < width_t, EGLint, _enable > :: value != true
                        && _narrowing < height_t, EGLint, _enable > :: value != true) {
                        TRACE (Trace::Information, (_T ("Possible narrowing detected!")));
                    }

                    // Update including some GLES preparations

                    using geom_t = decltype (std::declval < WPEFramework::Exchange::IComposition::IClient >().Geometry ());
                    using zorder_t = decltype (std::declval < WPEFramework::Exchange::IComposition::IClient >().ZOrder ());

                    using geom_x_t = decltype (geom_t::x);
                    using geom_y_t = decltype (geom_t::y);
                    using geom_w_t = decltype (geom_t::width);
                    using geom_h_t = decltype (geom_t::height);

                    using platform_w_t =  decltype (std::declval < decltype (_platform) >().Width ());
                    using platform_h_t =  decltype (std::declval < decltype (_platform) >().Height ());

                    using offset_x_t = decltype (GLES::offset_t::_x);
                    using offset_y_t = decltype (GLES::offset_t::_y);
                    using offset_z_t = decltype (GLES::offset_t::_z);

                    using scale_h_t = decltype (GLES::scale_t::_horiz);
                    using scale_v_t = decltype (GLES::scale_t::_vert);


                    using common_scale_h_t = std::common_type <scale_h_t, geom_w_t, platform_w_t> :: type;
                    using common_scale_v_t = std::common_type <scale_v_t, geom_h_t, platform_h_t> :: type;

                    // Narrowing is not allowed
                    static_assert (_narrowing <common_scale_h_t, scale_h_t, true> :: value != false);
                    static_assert (_narrowing <common_scale_v_t, scale_v_t, true> :: value != false);


                    geom_t _geom = _client->Geometry ();


                    GLES::scale_t _scale = { static_cast <scale_h_t> ( static_cast <common_scale_h_t> (_geom.width) / static_cast <common_scale_h_t> (_platform.Width ()) ), static_cast <scale_v_t> ( static_cast <common_scale_v_t> (_geom.height) / static_cast <common_scale_v_t> (_platform.Height ()) ) };


                    using common_offset_x_t = std::common_type <scale_h_t, geom_x_t, offset_x_t> :: type;
                    using common_offset_y_t = std::common_type <scale_v_t, geom_y_t, offset_y_t> :: type;
                    using common_offset_z_t = std::common_type <zorder_t, decltype (WPEFramework::Exchange::IComposition::minZOrder), decltype (WPEFramework::Exchange::IComposition::maxZOrder), offset_z_t> :: type;


                    // Narrowing is not allowed
                    static_assert (_narrowing <common_offset_x_t, offset_x_t, true> :: value != false);
                    static_assert (_narrowing <common_offset_y_t, offset_y_t, true> :: value != false);
                    static_assert (_narrowing <common_offset_z_t, offset_z_t, true> :: value != false);


                    zorder_t _zorder = _client->ZOrder ();

// TODO: z-ordering should support the minimal representable resolution

                    static_assert ( ( static_cast <common_offset_z_t> (WPEFramework::Exchange::IComposition::maxZOrder) - static_cast <common_offset_z_t> (WPEFramework::Exchange::IComposition::minZOrder) ) > static_cast <common_offset_z_t> (0));

                    GLES::offset_t _offset = { static_cast <offset_x_t> ( static_cast <common_offset_x_t> (_scale._horiz) * static_cast <common_offset_x_t> (_geom.x) / static_cast <common_offset_x_t> (_geom.width) ), static_cast <offset_y_t> ( static_cast <common_offset_y_t> (_scale._vert) * static_cast <common_offset_y_t> (_geom.y) / static_cast <common_offset_y_t> (_geom.height)), static_cast <offset_z_t> (static_cast <common_offset_z_t> (_zorder) / ( static_cast <common_offset_z_t> (WPEFramework::Exchange::IComposition::maxZOrder) - static_cast <common_offset_z_t> (WPEFramework::Exchange::IComposition::minZOrder) )) };


                    using opacity_t = decltype (std::declval < WPEFramework::Exchange::IComposition::IClient >().Opacity ());

                    using opacity_a_t = decltype (GLES::opacity_t::_alpha);

                    using common_opacity_t = std::common_type <opacity_t, opacity_a_t, decltype (WPEFramework::Exchange::IComposition::minOpacity), decltype (WPEFramework::Exchange::IComposition::maxOpacity)> :: type;

                    // Narrowing is not allowed
                    static_assert (_narrowing <common_opacity_t, opacity_a_t, true> :: value != false);


                    opacity_t _opa = _client->Opacity ();

                    GLES::opacity_t _opacity = static_cast <opacity_a_t> ( static_cast <common_opacity_t> (_opa) / ( static_cast <common_opacity_t> (WPEFramework::Exchange::IComposition::maxOpacity) - static_cast <common_opacity_t> (WPEFramework::Exchange::IComposition::minOpacity) ) );


                    _ret = (    _gles.UpdateOffset ( _offset )
                            && _gles.UpdateScale ( _scale ) && _gles.UpdateOpacity ( _opacity )
                            && _egl.RenderWithoutSwap (std::bind (&GLES::RenderEGLImage, &_gles, std::cref (_surf._khr), _width, _height)) ) != false;
                }


                // Update the scene only if sufficient time has elapes

                if (_ret != false) {
                    // Limit rate to avoid free run if the Swap fails

                    // Signed and at least 45 bits
                    using milli_t = int32_t;

                    auto RefreshRateFromResolution = [] (Exchange::IComposition::ScreenResolution const resolution) -> milli_t {
                        // Assume 'unknown' rate equals 60 Hz
                        milli_t _rate = 60;

                        switch (resolution) {
                            case Exchange::IComposition::ScreenResolution_1080p24Hz : // 1920x1080 progressive @ 24 Hz
                                                                                        _rate = 24; break;
                            case Exchange::IComposition::ScreenResolution_720p50Hz  : // 1280x720 progressive @ 50 Hz
                            case Exchange::IComposition::ScreenResolution_1080i50Hz : // 1920x1080 interlaced @ 50 Hz
                            case Exchange::IComposition::ScreenResolution_1080p50Hz : // 1920x1080 progressive @ 50 Hz
                            case Exchange::IComposition::ScreenResolution_2160p50Hz : // 4K, 3840x2160 progressive @ 50 Hz
                                                                                        _rate = 50; break;
                            case Exchange::IComposition::ScreenResolution_480i      : // 720x480
                            case Exchange::IComposition::ScreenResolution_480p      : // 720x480 progressive
                            case Exchange::IComposition::ScreenResolution_720p      : // 1280x720 progressive
                            case Exchange::IComposition::ScreenResolution_1080p60Hz : // 1920x1080 progressive @ 60 Hz
                            case Exchange::IComposition::ScreenResolution_2160p60Hz : // 4K, 3840x2160 progressive @ 60 Hz
                            case Exchange::IComposition::ScreenResolution_Unknown   :   _rate = 60;
                        }

                        return _rate;
                    };

                    constexpr milli_t _milli = 1000;

// TODO: statis are allowed if resize is not supported
                    static Exchange::IComposition::ScreenResolution _resolution = Resolution();

                    static decltype (_milli) _rate = RefreshRateFromResolution ( _resolution );

                    static std::chrono::milliseconds _delay = std::chrono::milliseconds (_milli / _rate);


                    // Delay the (free running) loop
                    auto _start = std::chrono::steady_clock::now ();


                    _ret = _egl.Render (std::bind (&GLES::RenderScene, &_gles, WidthFromResolution (_resolution), HeightFromResolution (_resolution), [] (GLES::texture_t left, GLES::texture_t right) -> GLES::valid_t { GLES::valid_t _ret = left._offset._z > right._offset._z; return _ret; } ), true ) != false;

                    if (_ret != false) {
                        ModeSet::BufferInfo _bufferInfo = { _natives.Surface (), nullptr, 0 };
                        /* void */ _platform.Swap (_bufferInfo);
                    }


                    auto _end = std::chrono::steady_clock::now ();

                    auto _duration = std::chrono::duration_cast < std::chrono::milliseconds > (_end - _start);

                    if (_duration.count () < _delay.count ()) {
                        SleepMs (_delay.count () - _duration.count ());
                    }
                }
            }

            return _ret;
        }

        //
        // Echange::IComposition
        // ==================================================================================================================

        static uint32_t WidthFromResolution(const ScreenResolution resolution) {
            // Asumme an invalid width equals 0
            uint32_t _width = 0;

            switch (resolution) {
                case ScreenResolution_480p      : // 720x480
                                                    _width = 720; break;
                case ScreenResolution_720p      : // 1280x720 progressive
                case ScreenResolution_720p50Hz  : // 1280x720 @ 50 Hz
                                                    _width = 1280; break;
                case ScreenResolution_1080p24Hz : // 1920x1080 progressive @ 24 Hz
                case ScreenResolution_1080i50Hz : // 1920x1080 interlaced  @ 50 Hz
                case ScreenResolution_1080p50Hz : // 1920x1080 progressive @ 50 Hz
                case ScreenResolution_1080p60Hz : // 1920x1080 progressive @ 60 Hz
                                                    _width = 1920; break;
                case ScreenResolution_2160p50Hz : // 4K, 3840x2160 progressive @ 50 Hz
                case ScreenResolution_2160p60Hz : // 4K, 3840x2160 progressive @ 60 Hz
                                                    _width = 2160; break;
                case ScreenResolution_480i      : // Unknown according to the standards (?)
                case ScreenResolution_Unknown   :
                default                         : _width = 0;
            }

            return _width;
        }

        static uint32_t HeightFromResolution(const ScreenResolution resolution) {
            // Asumme an invalid height equals 0
            uint32_t _height = 0;

            switch (resolution) {
                case ScreenResolution_480i      :
                case ScreenResolution_480p      : _height = 480; break;
                case ScreenResolution_720p      :
                case ScreenResolution_720p50Hz  : _height =720; break;
                case ScreenResolution_1080p24Hz :
                case ScreenResolution_1080i50Hz :
                case ScreenResolution_1080p50Hz :
                case ScreenResolution_1080p60Hz : _height = 1080; break;
                case ScreenResolution_2160p50Hz :
                case ScreenResolution_2160p60Hz : _height = 2160; break;
                case ScreenResolution_Unknown   :
                default                         : _height = 0;
            }

            return _height;
        }

        static ScreenResolution ResolutionFromHeightWidth(const uint32_t height, const uint32_t width) {
            // Given the options, the refresh rate is also important so the only sensible value is 'unknown'
            return Exchange::IComposition::ScreenResolution_Unknown;
        }

        uint32_t Configure(PluginHost::IShell* service) override
        {
            uint32_t result = Core::ERROR_NONE;
            _service = service;

            string configuration(service->ConfigLine());
            Config config;
            config.FromString(service->ConfigLine());

            _engine = Core::ProxyType<RPC::InvokeServer>::Create(&Core::IWorkerPool::Instance());
            _externalAccess = new ExternalAccess(*this, Core::NodeId(config.Connector.Value().c_str()), service->ProxyStubPath(), _engine);

            if ((_externalAccess->IsListening() == true)) {
                _port = config.Port.Value();
                PlatformReady();
            } else {
                delete _externalAccess;
                _externalAccess = nullptr;
                _engine.Release();
                TRACE(Trace::Error, (_T("Could not report PlatformReady as there was a problem starting the Compositor RPC %s"), _T("server")));
                result = Core::ERROR_OPENING_FAILED;
            }
            return result;
        }
        void Register(Exchange::IComposition::INotification* notification) override
        {
            _adminLock.Lock();
            ASSERT(std::find(_observers.begin(), _observers.end(), notification) == _observers.end());
            notification->AddRef();
            _observers.push_back(notification);

            _clients.Visit([=](const string& name, const Core::ProxyType<ClientSurface>& element){
                notification->Attached(name, &(*element));
            });

            _adminLock.Unlock();
        }
        void Unregister(Exchange::IComposition::INotification* notification) override
        {
            _adminLock.Lock();
            std::list<Exchange::IComposition::INotification*>::iterator index(std::find(_observers.begin(), _observers.end(), notification));
            ASSERT(index != _observers.end());
            if (index != _observers.end()) {
                _observers.erase(index);
                notification->Release();
            }
            _adminLock.Unlock();
        }
        void Attached(const string& name, IClient* client) {
            _adminLock.Lock();
            for(auto& observer : _observers) {
                observer->Attached(name, client);
            }
            _adminLock.Unlock();
        }
        void Detached(const string& name) {
            _adminLock.Lock();

            // Clean up client that leaves prematurely

            /* ProxyType <> */ auto _client = _clients.Find (name);

            if (_client.IsValid () != false) {
                EGL::img_t const _img = _client->Surface ()._khr;

                /* valid_t */ _gles.SkipEGLImageFromScene (_img);

                /* surf_t */ _client->Surface ( EGL::DestroyImage ( _egl, _client->Surface () ));
            }

            for(auto& observer : _observers) {
                observer->Detached(name);
            }
            _adminLock.Unlock();
        }

        //
        // Echange::IComposition::IDisplay
        // ==================================================================================================================
        RPC::instance_id Native () const override {
            using class_t = std::remove_reference < decltype (_natives) > ::type;
            using return_t = decltype ( std::declval < class_t > ().Display () );

            // The EGL API uses const very sparsely, and here, a const is also not expected
            using return_no_const_t  = _remove_const < return_t > :: type;

            static_assert ( (std::is_convertible < return_no_const_t, EGLNativeDisplayType > :: value) != false);
            static_assert ( (std::is_convertible < decltype (EGL_DEFAULT_DISPLAY), EGLNativeDisplayType > :: value) != false);
            // Likely to almost always fail
//            static_assert ( (std::is_convertible < return_no_const_t, RPC::instance_id > :: value) != false);

            EGLNativeDisplayType result ( static_cast < EGLNativeDisplayType > ( EGL_DEFAULT_DISPLAY ) );

            if (_natives.Valid () != false) {
                // Just remove the unexpected const if it exist
                result = static_cast < EGLNativeDisplayType > (const_cast < return_no_const_t > ( _natives.Display () ));
            }
            else {
                TRACE (Trace::Error, (_T ("The native display (id) might be invalid / unsupported. Using the EGL default display instead!")));
            }

            return reinterpret_cast < RPC::instance_id > ( result );
        }

        string Port() const override {
            return (_port);
        }
        IClient* CreateClient(const string& name, const uint32_t width, const uint32_t height) override {
            IClient* client = nullptr;

                Core::ProxyType<ClientSurface> object = _clients. template Instance(
                             name,
                             _platform,
                             *this,
                             name,
                             width,
                             height);

                ASSERT(object.IsValid() == true);

                if( object.IsValid() == true ) {
                    client = &(*object);
                    Attached (name, client);
                }

            if (client == nullptr) {
                TRACE(Trace::Error, (_T("Unable to create the ClientSurface with name %s"), name.c_str()));
            }
            else {
                _dma = new DMATransfer (* this);

                if (_dma == nullptr || _dma->Valid () != true) {
                    TRACE (Trace::Error, (_T ("DMA transfers are not supported.")));

                    delete _dma;
                    _dma = nullptr;
                }
                else {
                    _dma->Run ();
                }
            }

            return (client);
        }

        Exchange::IComposition::ScreenResolution Resolution() const override
        {
            Exchange::IComposition::ScreenResolution _resolution = WPEFramework::Exchange::IComposition::ScreenResolution_Unknown;

            decltype ( std::declval <ModeSet> ().Width () ) _width = _platform.Width (); silence (_width);
            decltype ( std::declval <ModeSet> ().Height () ) _height = _platform.Height ();

// TODO: This might not be the whole story to determine progressive versus interlaced

            decltype ( std::declval <ModeSet> ().RefreshRate () ) _vrefresh = _platform.RefreshRate ();
            decltype ( std::declval <ModeSet> ().Interlaced () ) _interlaced = _platform.Interlaced ();

            if (_interlaced != true) {
                switch (_height) {
                    case 480    :   {
                                        _resolution = ScreenResolution_480p;
                                        break;
                                    }
                    case 720    :   {
                                        _resolution = _vrefresh != 50 ? ScreenResolution_720p : ScreenResolution_720p50Hz;
                                        break;
                                    }
                    case 1080   :   {
                                        switch (_vrefresh) {
                                            case 24 : _resolution = ScreenResolution_1080p24Hz; break;
                                            case 50 : _resolution = ScreenResolution_1080p50Hz; break;
                                            case 60 : _resolution = ScreenResolution_1080p60Hz; break;
                                            default : _resolution = ScreenResolution_Unknown;
                                        }
                                        break;
                                    }
                    case 2160   :   {
                                        switch (_vrefresh) {
                                            case 50 : _resolution = ScreenResolution_2160p50Hz; break;
                                            case 60 : _resolution = ScreenResolution_2160p60Hz; break;
                                            default : _resolution = ScreenResolution_Unknown;
                                        }
                                        break;
                                    }
                    default     :   {
                                        _resolution = ScreenResolution_Unknown;
                                    }
                }
            }
            else {
                switch (_height) {
                    case 480    :   {
                                        _resolution = ScreenResolution_480i;
                                        break;
                                    }
                    case 1080   :   {
                                        _resolution = _vrefresh != 50 ? ScreenResolution_Unknown : ScreenResolution_1080i50Hz;
                                        break;
                                    }
                    default     :   {
                                        _resolution = ScreenResolution_Unknown;
                                    }
                }
            }

            return _resolution;

        }
        uint32_t Resolution(const Exchange::IComposition::ScreenResolution format) override
        {
            TRACE(Trace::Error, (_T("Could not set screenresolution to %s. Not supported for Rapberry Pi compositor"), Core::EnumerateType<Exchange::IComposition::ScreenResolution>(format).Data()));
            return (Core::ERROR_UNAVAILABLE);
        }

    private:
        void PlatformReady()
        {
            PluginHost::ISubSystem* subSystems(_service->SubSystems());
            ASSERT(subSystems != nullptr);
            if (subSystems != nullptr) {
                subSystems->Set(PluginHost::ISubSystem::PLATFORM, nullptr);
                subSystems->Set(PluginHost::ISubSystem::GRAPHICS, nullptr);
                subSystems->Release();
            }
        }

    private:

        using ClientContainer = Core::ProxyMapType<string, ClientSurface>;

        mutable Core::CriticalSection _adminLock;
        PluginHost::IShell* _service;
        Core::ProxyType<RPC::InvokeServer> _engine;
        ExternalAccess* _externalAccess;
        std::list<Exchange::IComposition::INotification*> _observers;
        ClientContainer _clients;
        string _port;
        ModeSet _platform;

        DMATransfer * _dma;
        Natives _natives;
        EGL _egl;
        GLES _gles;
        std::mutex _clientLock;
    };

    // ODR-use
    /* static */ constexpr void * const CompositorImplementation::EGL::EGL_NO_CONFIG;

    SERVICE_REGISTRATION(CompositorImplementation, 1, 0);

    ClientSurface::ClientSurface(ModeSet& modeSet, CompositorImplementation& compositor, const string& name, const uint32_t width, const uint32_t height)
        : _nativeSurface { nullptr, -1, WPEFramework::Plugin::EGL::InvalidImage () }
        , _modeSet(modeSet)
        , _compositor(compositor)
        , _name(name)
        , _opacity(Exchange::IComposition::maxOpacity)
        , _layer(0)
        , _destination( { 0, 0, width, height } ) {

        using surface_t = decltype (_nativeSurface._buf);
        using class_t = std::remove_reference < decltype (_modeSet) > ::type;
        using return_t = decltype ( std::declval < class_t > ().CreateBufferObject (width, height) );

        static_assert (std::is_same < surface_t, return_t> :: value != false);

// TODO: The internal scan out flag might not be appropriate
        _nativeSurface._buf = _modeSet.CreateBufferObject (width, height);

        if (_nativeSurface.Valid () != true) {
            TRACE (Trace::Error, (_T ("A ClientSurface cannot be created for %s"), name.c_str ()));
        }
        else {
            _nativeSurface._fd = gbm_bo_get_fd (_nativeSurface._buf);

            if (_nativeSurface.DMAComplete () != true) {
                TRACE (Trace::Error, (_T ("The created ClientSurface for %s is not suitable for DMA."), name.c_str ()));
            }
        }
    }

    ClientSurface::~ClientSurface() {

        // Part of the client is cleaned up via the detached (hook)

        _compositor.Detached(_name);

        if (_nativeSurface._fd != -1) {
            /* int */ close (_nativeSurface._fd);
        }

        if (_nativeSurface.Valid () != false) {
            _modeSet.DestroyBufferObject (_nativeSurface._buf);
        }

        _nativeSurface = { nullptr, -1 , WPEFramework::Plugin::EGL::InvalidImage () };

    }

    void ClientSurface::ScanOut () {
        _compositor.CompositeFor (_name);
    }

} // namespace Plugin
} // namespace WPEFramework
