/* Orchid - WebRTC P2P VPN Market (on Ethereum)
 * Copyright (C) 2017-2019  The Orchid Authors
*/

/* GNU Affero General Public License, Version 3 {{{ */
/*
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU Affero General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.

 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU Affero General Public License for more details.

 * You should have received a copy of the GNU Affero General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
**/
/* }}} */


#include <cppcoro/async_mutex.hpp>

#include <lwip/err.h>
#include <lwip/ip.h>
#include <lwip/netifapi.h>
#include <lwip/tcp.h>
#include <lwip/tcpip.h>
#include <lwip/udp.h>

#include <p2p/base/basic_packet_socket_factory.h>

#include "dns.hpp"
#include "event.hpp"
#include "lwip.hpp"
#include "manager.hpp"
#include "remote.hpp"

#define orc_lwipcall(call, expr) ({ \
    const auto _status(call expr); \
    orc_assert_(_status == ERR_OK, "lwip " << #call << " #" << int(_status) << " \"" << lwip_strerr(_status) << "\""); \
_status; })

extern "C" struct netif *hook_ip4_route_src(const ip4_addr_t *src, const ip4_addr_t *dest)
{
    orc_insist(src != nullptr);
    struct netif *netif;
    NETIF_FOREACH(netif) {
        if (netif_is_up(netif) && netif_is_link_up(netif) && ip4_addr_cmp(src, netif_ip4_addr(netif))) {
            return netif;
        }
    }
    return nullptr;
}

namespace orc {

class Reference {
  private:
    pbuf *buffer_;

  public:
    Reference() :
        buffer_(nullptr)
    {
    }

    Reference(pbuf *buffer) :
        buffer_(buffer)
    {
        pbuf_ref(buffer_);
    }

    Reference(const Reference &other) = delete;

    Reference(Reference &&other) noexcept :
        buffer_(other.buffer_)
    {
        other.buffer_ = nullptr;
    }

    ~Reference() {
        if (buffer_ != nullptr)
            pbuf_free(buffer_);
    }

    operator pbuf *() const {
        return buffer_;
    }

    pbuf *operator ->() const {
        return buffer_;
    }
};

class Chain :
    public Buffer
{
  private:
    Reference buffer_;

  public:
    Chain(const Buffer &data) :
        buffer_(pbuf_alloc(PBUF_RAW, data.size(), PBUF_RAM))
    {
        u16_t offset(0);
        data.each([&](const uint8_t *data, size_t size) {
            orc_lwipcall(pbuf_take_at, (buffer_, data, size, offset));
            offset += size;
            return true;
        });
    }

    Chain(pbuf *buffer) :
        buffer_(buffer)
    {
    }

    operator pbuf *() const {
        return buffer_;
    }

    bool each(const std::function<bool (const uint8_t *, size_t)> &code) const override {
        for (pbuf *buffer(buffer_); ; buffer = buffer->next) {
            orc_assert(buffer != nullptr);
            if (!code(static_cast<const uint8_t *>(buffer->payload), buffer->len))
                return false;
            if (buffer->tot_len == buffer->len) {
                orc_assert(buffer->next == nullptr);
                return true;
            }
        }
    }
};

class Core {
  public:
    Core() {
        sys_lock_tcpip_core();
    }

    ~Core() {
        sys_unlock_tcpip_core();
    }
};

// XXX: none of this calls Stop

class RemoteCommon {
  protected:
    udp_pcb *pcb_;

    virtual void Land(const Buffer &data, const Socket &socket) = 0;

    RemoteCommon(const ip4_addr_t &host) {
        Core core;
        pcb_ = udp_new();
        orc_assert(pcb_ != nullptr);
        orc_lwipcall(udp_bind, (pcb_, &host, 0));
    }

    ~RemoteCommon() {
        Core core;
        udp_remove(pcb_);
    }

  public:
    operator udp_pcb *() {
        return pcb_;
    }

    void Open(const Core &core) {
        udp_recv(pcb_, [](void *arg, udp_pcb *pcb, pbuf *data, const ip4_addr_t *host, u16_t port) noexcept {
            static_cast<RemoteCommon *>(arg)->Land(Chain(data), Socket(*host, port));
            pbuf_free(data);
        }, this);
    }

    void Shut() noexcept {
        Core core;
        udp_disconnect(pcb_);
    }
};

class RemoteAssociation :
    public Pump<Buffer>,
    public RemoteCommon
{
  protected:
    void Land(const Buffer &data, const Socket &socket) override {
        Pump::Land(data);
    }

  public:
    RemoteAssociation(BufferDrain *drain, const ip4_addr_t &host) :
        Pump(drain),
        RemoteCommon(host)
    {
    }

    void Open(const ip4_addr_t &host, uint16_t port) {
        Core core;
        RemoteCommon::Open(core);
        orc_lwipcall(udp_connect, (pcb_, &host, port));
    }

    task<void> Shut() noexcept override {
        RemoteCommon::Shut();
        Pump::Stop();
        co_await Pump::Shut();
    }

    task<void> Send(const Buffer &data) override {
        Core core;
        orc_lwipcall(udp_send, (pcb_, Chain(data)));
        co_return;
    }
};

class RemoteOpening final :
    public Opening,
    public RemoteCommon
{
  protected:
    void Land(const Buffer &data, const Socket &socket) override {
        drain_->Land(data, socket);
    }

  public:
    RemoteOpening(BufferSewer *drain, const ip4_addr_t &host) :
        Opening(drain),
        RemoteCommon(host)
    {
    }

    Socket Local() const override {
        return Socket(pcb_->local_ip, pcb_->local_port);
    }

    void Open() {
        Core core;
        RemoteCommon::Open(core);
    }

    task<void> Shut() noexcept override {
        RemoteCommon::Shut();
        Opening::Stop();
        co_await Opening::Shut();
    }

    task<void> Send(const Buffer &data, const Socket &socket) override {
        ip4_addr_t address(socket.Host());
        Core core;
        orc_lwipcall(udp_sendto, (pcb_, Chain(data), &address, socket.Port()));
        co_return;
    }
};

class RemoteConnection final :
    public Pump<Buffer>
{
  protected:
    tcp_pcb *pcb_;
    Transfer<err_t> opened_;

    cppcoro::async_mutex send_;
    cppcoro::async_auto_reset_event ready_;

  public:
    RemoteConnection(BufferDrain *drain, const ip4_addr_t &host) :
        Pump(drain)
    {
        Core core;
        pcb_ = tcp_new();
        orc_assert(pcb_ != nullptr);
        tcp_arg(pcb_, this);
        orc_lwipcall(tcp_bind, (pcb_, &host, 0));
    }

    ~RemoteConnection() override {
        Core core;
        if (pcb_ != nullptr)
            orc_except({ orc_lwipcall(tcp_close, (pcb_)); });
    }

    task<void> Open(const ip4_addr_t &host, uint16_t port) {
        { Core core;
            tcp_recv(pcb_, [](void *arg, tcp_pcb *pcb, pbuf *data, err_t error) noexcept -> err_t {
                const auto self(static_cast<RemoteConnection *>(arg));
                orc_insist(pcb == self->pcb_);

                // XXX: convert error
                orc_insist(error == ERR_OK);

                if (data == nullptr)
                    self->Stop();
                else {
                    const Chain chain(data);
                    self->Land(chain);
                    tcp_recved(pcb, chain.size());
                    pbuf_free(data);
                }

                return ERR_OK;
            });

            tcp_err(pcb_, [](void *arg, err_t error) noexcept {
                const auto self(static_cast<RemoteConnection *>(arg));
                self->pcb_ = nullptr;
                self->ready_.set();
                if (!self->opened_)
                    self->opened_(std::move(error));
                else self->Stop([&]() { switch (error) {
                    case ERR_ABRT:
                        return "ERR_ABRT";
                    case ERR_RST:
                        return "ERR_RST";
                    default:
                        orc_insist_(false, "lwip tcp_err #" << int(error) << " \"" << lwip_strerr(error) << "\"");
                } }());
            });

            tcp_sent(pcb_, [](void *arg, tcp_pcb *pcb, u16_t size) noexcept -> err_t {
                const auto self(static_cast<RemoteConnection *>(arg));
                orc_insist(pcb == self->pcb_);
                self->ready_.set();
                return ERR_OK;
            });

            orc_lwipcall(tcp_connect, (pcb_, &host, port, [](void *arg, tcp_pcb *pcb, err_t error) noexcept -> err_t {
                const auto self(static_cast<RemoteConnection *>(arg));
                self->opened_(std::move(error));
                return ERR_OK;
            }));
        }

        orc_lwipcall(co_await, opened_.Wait());
    }

    task<void> Shut() noexcept override {
        { Core core; if (pcb_ != nullptr) orc_except({ orc_lwipcall(tcp_shutdown, (pcb_, false, true)); }); }
        co_await Pump::Shut();
    }

    task<void> Send(const Buffer &data) override {
        const auto lock(co_await send_.scoped_lock_async());
        for (Window window(data); !window.done(); co_await ready_, co_await Schedule()) {
            Core core;
            orc_assert(pcb_ != nullptr);

            auto rest(tcp_sndbuf(pcb_));
            if (rest == 0)
                continue;
            const auto size(window.size());
            if (rest > size)
                rest = size;

            window.Take(rest, [&](const uint8_t *data, size_t size) {
                return Chunk(data, size, [&](const uint8_t *data, size_t size) {
                    orc_insist(size <= rest);
                    if (size > 0xffff)
                        size = 0xffff;
                    rest -= size;

                    orc_lwipcall(tcp_write, (pcb_, data, size, TCP_WRITE_FLAG_COPY));
                    // XXX: consider avoiding copies by holding the buffer until send completes
                    copied_ += size;
                    return size;
                });
            });
        }
    }
};

void Remote::Send(pbuf *buffer) {
    nest_.Hatch([&]() noexcept { return [this, data = Chain(buffer)]() -> task<void> {
        co_return co_await Inner()->Send(data);
    }; });
}

err_t Remote::Output(netif *interface, pbuf *buffer, const ip4_addr_t *destination) {
    static_cast<Remote *>(interface->state)->Send(buffer);
    return ERR_OK;
}

err_t Remote::Initialize(netif *interface) {
    interface->name[0] = 'o';
    interface->name[1] = 'r';
    interface->output = &Output;
    return ERR_OK;
}

void Remote::Land(const Buffer &data) {
    orc_assert(tcpip_inpkt(Chain(data), &interface_, interface_.input) == ERR_OK);
}

void Remote::Stop(const std::string &error) noexcept {
    netifapi_netif_set_link_down(&interface_);
    Origin::Stop();
}

Remote::Remote(const class Host &host) :
    Origin(std::make_unique<Assistant>(host)),
    host_(host)
{
    static bool setup(false);
    if (!setup) {
        tcpip_init(nullptr, nullptr);
        setup = true;
    }

    ip4_addr_t gateway; IP4_ADDR(&gateway, 10,7,0,1);
    ip4_addr_t address(host_);
    ip4_addr_t netmask; IP4_ADDR(&netmask, 255,255,255,0);

    orc_assert(netifapi_netif_add(&interface_, &address, &netmask, &gateway, this, &Initialize, &ip_input) == ERR_OK);
}

static uint8_t quad_(3);

Remote::Remote() :
    Remote({10,7,0,++quad_})
{
    type_ = typeid(*this).name();
}

Remote::~Remote() {
    netifapi_netif_remove(&interface_);
}

void Remote::Open() {
    netifapi_netif_set_up(&interface_);
    netifapi_netif_set_link_up(&interface_);
}

task<void> Remote::Shut() noexcept {
    co_await nest_.Shut();
    co_await Inner()->Shut();
    co_await Valve::Shut();
    netifapi_netif_set_down(&interface_);
}

class Host Remote::Host() {
    return host_;
}

rtc::Thread *Remote::Thread() {
    static std::unique_ptr<rtc::Thread> thread;
    if (thread == nullptr) {
        thread = std::make_unique<rtc::Thread>(std::make_unique<LwipSocketServer>());
        thread->SetName("Orchid WebRTC Remote", nullptr);
        thread->Start();
    }

    return thread.get();
}

rtc::BasicPacketSocketFactory &Remote::Factory() {
    static rtc::BasicPacketSocketFactory factory(Thread());
    return factory;
}

task<void> Remote::Associate(Sunk<> *sunk, const Socket &endpoint) {
    const auto association(sunk->Wire<RemoteAssociation>(host_));
    association->Open(endpoint.Host(), endpoint.Port());
    co_return;
}

task<Socket> Remote::Unlid(Sunk<BufferSewer, Opening> *sunk) {
    const auto opening(sunk->Wire<RemoteOpening>(host_));
    opening->Open();
    co_return opening->Local();
}

task<void> Remote::Connect(U<Stream> &stream, const Socket &endpoint) {
    auto reverted(std::make_unique<Sink<Reverted>>());
    const auto connection(reverted->Wire<RemoteConnection>(host_));
    stream = std::move(reverted);
    co_await connection->Open(endpoint.Host(), endpoint.Port());
}

}
