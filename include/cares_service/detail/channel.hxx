#ifndef __CARES_SERVICES_CHANNEL_HXX__
#define __CARES_SERVICES_CHANNEL_HXX__

#if defined(_WIN32) && !defined(__CYGWIN__)
    #define SET_SOCKERRNO(x) (WSASetLastError((int)(x)))
    struct iovec {
        void *iov_base;
        size_t iov_len;
    };
#else
    #define SET_SOCKERRNO(x) (errno = (x))
#endif

#include <memory>
#include <map>
#include <boost/asio.hpp>
#include <boost/variant.hpp>
#include <ares.h>

#include "error.hxx"
#include "resolve_mode.hxx"

namespace cares {
namespace detail {

inline std::shared_ptr<struct ares_socket_functions> GetSocketFunctions();
inline char *GetAresLookups();

inline ares_socket_t OpenSocket(int family, int type, int protocol, void *arg);
inline int CloseSocket(ares_socket_t fd, void *arg);
inline int ConnectSocket(ares_socket_t fd, const struct sockaddr *addr, ares_socklen_t len, void *arg);
inline ares_ssize_t ReadSocket(ares_socket_t fd, void *data, size_t data_len, int flags, struct sockaddr *addr, ares_socklen_t *addr_len, void *arg);
inline ares_ssize_t SendSocket(ares_socket_t fd, const struct iovec *data, int len, void *arg);

inline void SocketStateCb(void *arg, ares_socket_t fd, int readable, int writeable);

class Channel : public std::enable_shared_from_this<Channel> {
public:
    using resolve_mode = ::cares::detail::resolve_mode;

private:
    struct Socket : public std::enable_shared_from_this<Socket> {
        using tcp_type = boost::asio::ip::tcp::socket;
        using udp_type = boost::asio::ip::udp::socket;

        Socket(tcp_type tcp)
            : socket_(std::move(tcp)), is_tcp_(true) {
        }

        Socket(udp_type udp)
            : socket_(std::move(udp)), is_tcp_(false) {
        }

        Socket(Socket &&) = default;

        tcp_type &GetTcp() {
            return boost::get<tcp_type>(socket_);
        }

        udp_type &GetUdp() {
            return boost::get<udp_type>(socket_);
        }

        bool IsTcp() const {
            return is_tcp_;
        }

        ares_socket_t GetFd() {
            if (IsTcp()) {
                return GetTcp().native_handle();
            }
            return GetUdp().native_handle();
        }

        void Close() {
            if (IsTcp()) {
                GetTcp().close();
            } else {
                GetUdp().close();
            }
        }

        void Cancel() {
            if (IsTcp()) {
                GetTcp().cancel();
            } else {
                GetUdp().cancel();
            }
        }

        template<class Handler>
        void AsyncWaitRead(Handler &&cb) {
            auto self{shared_from_this()};
            auto handler = \
                [this, self, cb=std::move(cb)](boost::system::error_code ec) {
                    cb();
                    if (!ec) {
                        AsyncWaitRead(std::move(cb));
                    }
                };
            if (IsTcp()) {
                GetTcp().async_wait(tcp_type::wait_read, std::move(handler));
            } else {
                GetUdp().async_wait(udp_type::wait_read, std::move(handler));
            }
        }

        template<class Handler>
        void AsyncWaitWrite(Handler &&cb) {
            auto self{shared_from_this()};
            auto handler = \
                [this, self, cb=std::move(cb)](boost::system::error_code ec) {
                    cb();
                    if (!ec) {
                        AsyncWaitWrite(std::move(cb));
                    }
                };
            if (IsTcp()) {
                GetTcp().async_wait(tcp_type::wait_write, std::move(handler));
            } else {
                GetUdp().async_wait(udp_type::wait_write, std::move(handler));
            }
        }

        boost::variant<udp_type, tcp_type> socket_;
        bool is_tcp_;
    };
public:
    using AsyncCallback = std::function<void(boost::system::error_code, struct hostent *)>;
    using native_handle_type = ares_channel;

    Channel(const Channel &) = delete;
    explicit Channel(boost::asio::io_context &ios, boost::posix_time::time_duration timeout = boost::posix_time::millisec{3000})
        : context_(ios), strand_(context_),
          timer_(context_), timer_period_(timeout / 2),
          functions_(GetSocketFunctions()), request_count_(0),
          resolve_mode_(both) {

        struct ares_options option;
        memset(&option, 0, sizeof option);
        option.sock_state_cb = SocketStateCb;
        option.sock_state_cb_data = this;
        option.timeout = timeout.total_milliseconds();
        option.tries = 1;
        option.lookups = GetAresLookups();
        int mask = ARES_OPT_NOROTATE | ARES_OPT_TIMEOUTMS | ARES_OPT_SOCK_STATE_CB | ARES_OPT_TRIES | ARES_OPT_LOOKUPS;

        int ret = ::ares_init_options(&channel_, &option, mask);
        if (ret != ARES_SUCCESS) {
            boost::system::error_code ec{ret, error::get_category()};
            boost::throw_exception(
                boost::system::system_error{
                    ec, ::ares_strerror(ret)
                }
            );
        }

        ::ares_set_socket_functions(channel_, functions_.get(), this);
    }

    ~Channel() {
        Cancel();
        ::ares_destroy(channel_);
    }

    template<class Results, class Handler>
    void AsyncGetHostByName(const std::string &domain, std::shared_ptr<Results> result, std::shared_ptr<Handler> handler) {
        auto self{shared_from_this()};
        auto mode = GetResolveMode();
        auto remain_requests = std::make_shared<uint32_t>(1);

        if (mode != ipv6_only && mode != ipv4_only) {
            ++*remain_requests;
        }

        if (mode != ipv6_only) {
            AsyncGetHostByNameInternal(
                domain, AF_INET,
                std::bind(
                    &Channel::ResultHandler<Results, Handler>, self,
                    std::placeholders::_1, std::placeholders::_2,
                    result, handler, remain_requests
                )
            );
        }

        if (mode != ipv4_only) {
            AsyncGetHostByNameInternal(
                domain, AF_INET6,
                std::bind(
                    &Channel::ResultHandler<Results, Handler>, self,
                    std::placeholders::_1, std::placeholders::_2,
                    result, handler, remain_requests
                )
            );
        }
    }

    void Cancel() {
        ::ares_cancel(channel_);
        TimerStop();
    }

    void SetServerPortsCsv(const std::string &servers, boost::system::error_code &ec) {
        ec.clear();
        int ret = ::ares_set_servers_ports_csv(channel_, servers.c_str());
        if (ret != ARES_SUCCESS) {
            ec.assign(ret, error::get_category());
        }
    }

    void SetResolveMode(resolve_mode mode, boost::system::error_code &ec) {
        ec.clear();
        if (!is_valid_resolve_mode(mode)) {
            ec.assign(error::not_implemented, error::get_category());
            return;
        }
        resolve_mode_ = mode;
    }

    resolve_mode GetResolveMode() const {
        return resolve_mode_;
    }

    native_handle_type GetNativeHandle() {
        return channel_;
    }

private:
    struct ChannelComplete {
        std::shared_ptr<Channel> channel;
        AsyncCallback callback;
    };

    template<class Callback>
    void AsyncGetHostByNameInternal(const std::string &domain, int family, Callback &&cb) {
        auto comp = std::make_unique<ChannelComplete>();
        auto self{shared_from_this()};
        comp->channel = self;
        comp->callback = std::move(cb);
        ::ares_gethostbyname(channel_, domain.c_str(), family, &Channel::HostCallback, comp.release());
        boost::asio::post(
            strand_,
            [this, self]() {
                if (request_count_ == 0) {
                    TimerStart();
                }
                ++request_count_;
            }
        );
    }

    template<class Results, class Callback>
    void ResultHandler(boost::system::error_code ec, struct hostent *entries, std::shared_ptr<Results> &result, std::shared_ptr<Callback> cb, std::shared_ptr<uint32_t> req) {
        int family;
        bool need_prepend = false;
        auto mode = GetResolveMode();
        bool should_invoke_cb = false;
        --*req;
        switch (mode) {
        case unspecific:
            if (!result->IsEmpty()) {
                break;
            }
            if (!ec && result->IsEmpty()) {
                result->Append(entries);
            }
            if (!result->IsEmpty() || *req == 0) {
                should_invoke_cb = true;
            }
            break;

        case ipv4_first:
        case ipv6_first:
        case both:
            need_prepend = (mode != both) && result->LastFamily(family);
            need_prepend = \
                need_prepend && (family == (mode == ipv4_first ? AF_INET6 : AF_INET));
            if (!ec && !need_prepend) {
                result->Append(entries);
            } else if (!ec && need_prepend) {
                result->Prepend(entries);
            }
            if (*req == 0) {
                if (!result->IsEmpty()) {
                    ec.clear();
                }
                should_invoke_cb = true;
            }
            break;

        case ipv4_only:
        case ipv6_only:
            if (!ec) {
                result->Append(entries);
            }
            should_invoke_cb = true;
            break;

        default:
            assert(false);
            break;
        };
        if (should_invoke_cb) {
            boost::asio::post(
                context_,
                [cb, ec, result]() {
                    (*cb)(ec, *result);
                }
            );
        }
    }

    void TimerStart() {
        auto self{shared_from_this()};
        last_tick_ = boost::posix_time::microsec_clock::local_time();
        timer_.expires_from_now(timer_period_);
        timer_.async_wait(std::bind(&Channel::TimerCallback, self, std::placeholders::_1));
    }

    void TimerStop() {
        timer_.cancel();
    }

    void TimerCallback(boost::system::error_code ec) {
        auto self{shared_from_this()};
        auto now = boost::posix_time::microsec_clock::local_time();
        auto after = last_tick_ - now + timer_period_;
        if (after.is_negative() || after == boost::posix_time::millisec{0}) {
            last_tick_ = boost::posix_time::microsec_clock::local_time();
            ProcessFd(ARES_SOCKET_BAD, ARES_SOCKET_BAD);
            after = timer_period_;
        }
        if (!ec && request_count_) {
            timer_.expires_from_now(after);
            timer_.async_wait(std::bind(&Channel::TimerCallback, self, std::placeholders::_1));
        }
    }

    void ProcessFd(ares_socket_t rd, ares_socket_t wr) {
        auto self{shared_from_this()};
        boost::asio::post(
            strand_,
            [this, self, rd, wr]() {
                last_tick_ = boost::posix_time::microsec_clock::local_time();
                ::ares_process_fd(channel_, rd, wr);
            }
        );
    }

    static void HostCallback(void *arg, int status, int timeouts, struct hostent *hostent) {
        std::unique_ptr<ChannelComplete> comp;
        comp.reset(static_cast<ChannelComplete *>(arg));
        boost::system::error_code ec;
        if (status != ARES_SUCCESS) {
            ec.assign(status, error::get_category());
        }
        comp->callback(ec, hostent);
        auto channel = comp->channel;
        boost::asio::post(
            channel->strand_,
            [comp{std::move(comp)}]() {
                comp->channel->request_count_--;
                if (comp->channel->request_count_ == 0) {
                    comp->channel->TimerStop();
                }
            }
        );
    }

    boost::asio::io_context &context_;
    boost::asio::io_context::strand strand_;
    native_handle_type channel_;
    boost::asio::deadline_timer timer_;
    boost::posix_time::time_duration timer_period_;
    boost::posix_time::ptime last_tick_;
    std::shared_ptr<struct ares_socket_functions> functions_;
    std::map<ares_socket_t, std::shared_ptr<Socket>> sockets_;
    int64_t request_count_;
    resolve_mode resolve_mode_;

    friend ares_socket_t OpenSocket(int family, int type, int protocol, void *arg);
    friend int CloseSocket(ares_socket_t fd, void *arg);
    friend int ConnectSocket(ares_socket_t fd, const struct sockaddr *addr, ares_socklen_t len, void *arg);
    friend ares_ssize_t ReadSocket(ares_socket_t fd, void *data, size_t data_len, int flags, struct sockaddr *addr, ares_socklen_t *addr_len, void *arg);
    friend ares_ssize_t SendSocket(ares_socket_t fd, const struct iovec *data, int len, void *arg);
    friend void SocketStateCb(void *arg, ares_socket_t fd, int readable, int writeable);
};

std::shared_ptr<struct ares_socket_functions> GetSocketFunctions() {
    static std::shared_ptr<struct ares_socket_functions> funcs = \
        []() {
            auto result = std::make_shared<struct ares_socket_functions>();
            result->asocket   = OpenSocket;
            result->aclose    = CloseSocket;
            result->aconnect  = ConnectSocket;
            result->arecvfrom = ReadSocket;
            result->asendv    = SendSocket;
            return result;
        }();
    return funcs;
}

char *GetAresLookups() {
    static char lookups[] = "bf";
    return lookups;
}

ares_socket_t OpenSocket(int family, int type, int protocol, void *arg) {
    auto channel = static_cast<Channel *>(arg);
    auto &context = channel->context_;
    ares_socket_t result = ARES_SOCKET_BAD;
    boost::system::error_code ec;

    if (type == SOCK_STREAM) {
        boost::asio::ip::tcp::socket sock{context};
        auto af = (family == AF_INET) ? boost::asio::ip::tcp::v4() : boost::asio::ip::tcp::v6();
        sock.open(af, ec);
        if (ec) { goto __open_socket_final_state; }

        sock.non_blocking(true, ec);
        if (ec) { goto __open_socket_final_state; }

        result = sock.native_handle();
        auto ptr = std::make_shared<Channel::Socket>(std::move(sock));
        channel->sockets_.emplace(result, ptr);
    } else if (type == SOCK_DGRAM) {
        boost::asio::ip::udp::socket sock{context};
        auto af = (family == AF_INET) ? boost::asio::ip::udp::v4() : boost::asio::ip::udp::v6();
        sock.open(af, ec);
        if (ec) { goto __open_socket_final_state; }

        sock.non_blocking(true, ec);
        if (ec) { goto __open_socket_final_state; }

        result = sock.native_handle();
        auto ptr = std::make_shared<Channel::Socket>(std::move(sock));
        channel->sockets_.emplace(result, ptr);
    } else {
        assert(false);
    }

__open_socket_final_state:
    if (ec) {
        SET_SOCKERRNO(ec.value());
        return -1;
    }
    return result;
}

int CloseSocket(ares_socket_t fd, void *arg) {
    auto channel = static_cast<Channel *>(arg);
    auto &sockets = channel->sockets_;
    auto itr = sockets.find(fd);
    itr->second->Close();
    sockets.erase(itr);
    return 0;
}

int ConnectSocket(ares_socket_t fd, const struct sockaddr *addr, ares_socklen_t addr_len, void *arg) {
    auto channel = static_cast<Channel *>(arg);
    auto itr = channel->sockets_.find(fd);
    auto self{itr->second};
    boost::system::error_code ec;

    if (self->IsTcp()) {
        boost::asio::ip::tcp::endpoint ep;
        ep.resize(addr_len);
        memcpy(ep.data(), addr, addr_len);
        self->GetTcp().connect(ep, ec);
    } else {
        boost::asio::ip::udp::endpoint ep;
        ep.resize(addr_len);
        memcpy(ep.data(), addr, addr_len);
        self->GetUdp().connect(ep, ec);
    }
    SET_SOCKERRNO(ec.value());
    return (ec ? -1 : 0);
}

ares_ssize_t ReadSocket(ares_socket_t fd, void *data, size_t data_len, int flags, struct sockaddr *addr, ares_socklen_t *addr_len, void *arg) {
    auto channel = static_cast<Channel *>(arg);
    auto itr = channel->sockets_.find(fd);
    auto self{itr->second};
    boost::system::error_code ec;

    ares_ssize_t result = -1;
    if (self->IsTcp()) {
        auto &socket = self->GetTcp();
        result = socket.read_some(boost::asio::buffer(data, data_len), ec);
        if (!ec && addr) {
            auto ep = socket.remote_endpoint();
            *addr_len = ep.size();
            memcpy(addr, ep.data(), ep.size());
        }
    } else {
        auto &socket = self->GetUdp();
        boost::asio::ip::udp::endpoint ep;
        result = socket.receive_from(boost::asio::buffer(data, data_len), ep, flags, ec);
        if (!ec && addr) {
            *addr_len = ep.size();
            memcpy(addr, ep.data(), ep.size());
        }
    }
    SET_SOCKERRNO(ec.value());
    return (ec ? -1 : result);
}

ares_ssize_t SendSocket(ares_socket_t fd, const struct iovec *data, int len, void *arg) {
    auto channel = static_cast<Channel *>(arg);
    auto itr = channel->sockets_.find(fd);
    auto self{itr->second};
    boost::system::error_code ec;

    ares_ssize_t result = -1;
    std::vector<boost::asio::mutable_buffer> buf_seq;
    buf_seq.reserve(len);
    for (int i = 0; i < len; ++i) {
        buf_seq.push_back(boost::asio::buffer(data[i].iov_base, data[i].iov_len));
    }
    if (self->IsTcp()) {
        auto &socket = self->GetTcp();
        result = socket.write_some(std::move(buf_seq), ec);
    } else {
        auto &socket = self->GetUdp();
        result = socket.send(std::move(buf_seq), 0, ec);
    }
    SET_SOCKERRNO(ec.value());
    return (ec ? -1 : result);
}

void SocketStateCb(void *arg, ares_socket_t fd, int readable, int writeable) {
    auto channel = static_cast<Channel *>(arg);
    auto itr = channel->sockets_.find(fd);
    auto self{itr->second};

    self->Cancel();
    if (readable) {
        self->AsyncWaitRead(
            std::bind(
                &Channel::ProcessFd,
                channel->shared_from_this(),
                fd, ARES_SOCKET_BAD
            )
        );
    }
    if (writeable) {
        self->AsyncWaitWrite(
            std::bind(
                &Channel::ProcessFd,
                channel->shared_from_this(),
                ARES_SOCKET_BAD, fd
            )
        );
    }
}

} // namespace detail
} // namespace cares

#endif // __CARES_SERVICES_CHANNEL_HXX__

