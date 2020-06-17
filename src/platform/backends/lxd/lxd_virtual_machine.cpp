/*
 * Copyright (C) 2019-2020 Canonical, Ltd.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; version 3.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 *
 */

#include "lxd_virtual_machine.h"
#include "lxd_request.h"

#include <QJsonArray>
#include <QJsonDocument>

#include <multipass/exceptions/start_exception.h>
#include <multipass/format.h>
#include <multipass/ip_address.h>
#include <multipass/logging/log.h>
#include <multipass/network_access_manager.h>
#include <multipass/optional.h>
#include <multipass/utils.h>
#include <multipass/virtual_machine_description.h>
#include <multipass/vm_status_monitor.h>

#include <chrono>
#include <thread>

namespace mp = multipass;
namespace mpl = multipass::logging;
namespace mpu = multipass::utils;

using namespace std::literals::chrono_literals;

namespace
{
auto instance_state_for(const QString& name, mp::NetworkAccessManager* manager, const QUrl& url)
{
    auto json_reply = lxd_request(manager, "GET", url);
    auto metadata = json_reply["metadata"].toObject();
    mpl::log(mpl::Level::trace, name.toStdString(),
             fmt::format("Got LXD container state: {} is {}", name, metadata["status"].toString()));

    switch (metadata["status_code"].toInt(-1))
    {
    case 101: // Started
    case 103: // Running
    case 107: // Stopping
    case 111: // Thawed
        return mp::VirtualMachine::State::running;
    case 102: // Stopped
        return mp::VirtualMachine::State::stopped;
    case 106: // Starting
        return mp::VirtualMachine::State::starting;
    case 109: // Freezing
        return mp::VirtualMachine::State::suspending;
    case 110: // Frozen
        return mp::VirtualMachine::State::suspended;
    case 104: // Cancelling
    case 108: // Aborting
        return mp::VirtualMachine::State::unknown;
    default:
        mpl::log(mpl::Level::error, name.toStdString(),
                 fmt::format("Got unexpected LXD state: {} ({})", metadata["status_code"].toString(),
                             metadata["status"].toInt()));
        return mp::VirtualMachine::State::unknown;
    }
}

mp::optional<mp::IPAddress> get_ip_for(const QString& name, mp::NetworkAccessManager* manager, const QUrl& url)
{
    const auto json_state = lxd_request(manager, "GET", url);
    const auto addresses =
        json_state["metadata"].toObject()["network"].toObject()["eth0"].toObject()["addresses"].toArray();

    for (const auto address : addresses)
    {
        if (address.toObject()["family"].toString() == "inet")
        {
            return mp::optional<mp::IPAddress>{address.toObject()["address"].toString().toStdString()};
        }
    }

    mpl::log(mpl::Level::trace, name.toStdString(), fmt::format("IP for {} not found...", name));
    return mp::nullopt;
}

mp::MemorySize get_minimum_disk_size(const mp::MemorySize& requested_disk_size)
{
    mp::MemorySize lxd_min_disk_size{"10G"};

    if (requested_disk_size > lxd_min_disk_size)
    {
        return requested_disk_size;
    }
    else
    {
        return lxd_min_disk_size;
    }
}

} // namespace

mp::LXDVirtualMachine::LXDVirtualMachine(const VirtualMachineDescription& desc, VMStatusMonitor& monitor,
                                         NetworkAccessManager* manager, const QUrl& base_url)
    : VirtualMachine{desc.vm_name},
      name{QString::fromStdString(desc.vm_name)},
      username{desc.ssh_username},
      monitor{&monitor},
      base_url{base_url},
      manager{manager}
{
    try
    {
        current_state();
    }
    catch (const LXDNotFoundException& e)
    {
        mpl::log(mpl::Level::debug, name.toStdString(),
                 fmt::format("Creating container with stream: {}, id: {}", desc.image.stream_location, desc.image.id));

        QJsonObject config{{"limits.cpu", QString::number(desc.num_cores)},
                           {"limits.memory", QString::number(desc.mem_size.in_bytes())}};

        if (!desc.meta_data_config.IsNull())
            config["user.meta-data"] = QString::fromStdString(mpu::emit_cloud_config(desc.meta_data_config));

        if (!desc.vendor_data_config.IsNull())
            config["user.vendor-data"] = QString::fromStdString(mpu::emit_cloud_config(desc.vendor_data_config));

        if (!desc.user_data_config.IsNull())
            config["user.user-data"] = QString::fromStdString(mpu::emit_cloud_config(desc.user_data_config));

        QJsonObject devices{
            {"config", QJsonObject{{"source", "cloud-init:config"}, {"type", "disk"}}},
            {"root", QJsonObject{{"path", "/"},
                                 {"pool", "default"},
                                 {"size", QString::number(get_minimum_disk_size(desc.disk_space).in_bytes())},
                                 {"type", "disk"}}}};

        QJsonObject virtual_machine{
            {"name", name},
            {"config", config},
            {"devices", devices},
            {"source", QJsonObject{{"type", "image"},
                                   {"mode", "pull"},
                                   {"server", QString::fromStdString(desc.image.stream_location)},
                                   {"protocol", "simplestreams"},
                                   {"fingerprint", QString::fromStdString(desc.image.id)}}}};

        auto json_reply = lxd_request(manager, "POST", QUrl(QString("%1/virtual-machines").arg(base_url.toString())),
                                      virtual_machine);
        mpl::log(mpl::Level::trace, name.toStdString(),
                 fmt::format("Got LXD creation reply: {}", QJsonDocument(json_reply).toJson()));

        if (json_reply["metadata"].toObject()["class"] == QStringLiteral("task") &&
            json_reply["status_code"].toInt(-1) == 100)
        {
            QUrl task_url(QString("%1/operations/%2")
                              .arg(base_url.toString())
                              .arg(json_reply["metadata"].toObject()["id"].toString()));

            // Instead of polling, need to use websockets to get events
            while (true)
            {
                try
                {
                    auto task_reply = lxd_request(manager, "GET", task_url);

                    if (task_reply["error_code"].toInt(-1) != 0)
                    {
                        break;
                    }

                    auto status_code = task_reply["metadata"].toObject()["status_code"].toInt(-1);
                    if (status_code == 200)
                    {
                        break;
                    }
                    else
                    {
                        std::this_thread::sleep_for(1s);
                    }
                }
                // Implies the task is finished
                catch (const LXDNotFoundException& e)
                {
                    break;
                }
            }
        }

        current_state();
    }
}

mp::LXDVirtualMachine::~LXDVirtualMachine()
{
}

void mp::LXDVirtualMachine::start()
{
    auto present_state = current_state();

    if (present_state == State::running)
        return;

    if (present_state == State::suspending)
        throw std::runtime_error("cannot start the instance while suspending");

    if (state == State::suspended)
    {
        mpl::log(mpl::Level::info, vm_name, fmt::format("Resuming from a suspended state"));
        request_state("unfreeze");
    }
    else
    {
        request_state("start");
    }

    state = State::starting;
    update_state();
}

void mp::LXDVirtualMachine::stop()
{
    std::unique_lock<decltype(state_mutex)> lock{state_mutex};
    auto present_state = current_state();

    if (present_state == State::running || present_state == State::delayed_shutdown)
    {
        auto state_task = request_state("stop");
        if (state_task["metadata"].toObject()["class"] == QStringLiteral("task") &&
            state_task["status_code"].toInt(-1) == 100)
        {
            QUrl task_url(QString("%1/operations/%2/wait")
                              .arg(base_url.toString())
                              .arg(state_task["metadata"].toObject()["id"].toString()));
            lxd_request(manager, "GET", task_url);
        }

        state = State::stopped;
        port = mp::nullopt;
    }
    else if (present_state == State::starting)
    {
        state = State::off;
        request_state("stop");
        state_wait.wait(lock, [this] { return state == State::stopped; });
        port = mp::nullopt;
    }
    else if (present_state == State::suspended)
    {
        mpl::log(mpl::Level::info, vm_name, fmt::format("Ignoring shutdown issued while suspended"));
    }

    update_state();
    lock.unlock();
}

void mp::LXDVirtualMachine::shutdown()
{
    stop();
}

void mp::LXDVirtualMachine::suspend()
{
    throw std::runtime_error("suspend is currently not supported");
}

mp::VirtualMachine::State mp::LXDVirtualMachine::current_state()
{
    auto present_state = instance_state_for(name, manager, state_url());

    if ((state == State::delayed_shutdown && present_state == State::running) || state == State::starting)
        return state;

    state = present_state;
    return state;
}

int mp::LXDVirtualMachine::ssh_port()
{
    return 22;
}

void mp::LXDVirtualMachine::ensure_vm_is_running()
{
    std::lock_guard<decltype(state_mutex)> lock{state_mutex};
    if (current_state() == State::off)
    {
        // Have to set 'stopped' here so there is an actual state change to compare to for
        // the cond var's predicate
        state = State::stopped;
        state_wait.notify_all();
        throw mp::StartException(vm_name, "Instance shutdown during start");
    }
}

void mp::LXDVirtualMachine::update_state()
{
    monitor->persist_state_for(vm_name, state);
}

std::string mp::LXDVirtualMachine::ssh_hostname()
{
    if (!ip)
    {
        auto action = [this] {
            ensure_vm_is_running();
            ip = get_ip_for(name, manager, state_url());
            if (ip)
            {
                return mpu::TimeoutAction::done;
            }
            else
            {
                return mpu::TimeoutAction::retry;
            }
        };
        auto on_timeout = [] { throw std::runtime_error("failed to determine IP address"); };
        mpu::try_action_for(on_timeout, std::chrono::minutes(2), action);
    }

    return ip.value().as_string();
}

std::string mp::LXDVirtualMachine::ssh_username()
{
    return username;
}

std::string mp::LXDVirtualMachine::ipv4()
{
    if (!ip)
    {
        ip = get_ip_for(name, manager, state_url());
        if (!ip)
            return "UNKNOWN";
    }

    return ip.value().as_string();
}

std::string mp::LXDVirtualMachine::ipv6()
{
    return {};
}

void mp::LXDVirtualMachine::wait_until_ssh_up(std::chrono::milliseconds timeout)
{
    mpu::wait_until_ssh_up(this, timeout, std::bind(&LXDVirtualMachine::ensure_vm_is_running, this));
}

const QUrl mp::LXDVirtualMachine::url()
{
    return QString("%1/virtual-machines/%2").arg(base_url.toString()).arg(name);
}

const QUrl mp::LXDVirtualMachine::state_url()
{
    return url().toString() + "/state";
}

const QJsonObject mp::LXDVirtualMachine::request_state(const QString& new_state)
{
    const QJsonObject state_json{{"action", new_state}};

    return lxd_request(manager, "PUT", state_url(), state_json, 5000);
}
