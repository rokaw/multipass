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

#ifndef MULTIPASS_LXD_VIRTUAL_MACHINE_FACTORY_H
#define MULTIPASS_LXD_VIRTUAL_MACHINE_FACTORY_H

#include "lxd_request.h"

#include <multipass/network_access_manager.h>
#include <multipass/virtual_machine_factory.h>

#include <QUrl>

namespace multipass
{
class LXDVirtualMachineFactory final : public VirtualMachineFactory
{
public:
    explicit LXDVirtualMachineFactory(const Path& data_dir, const QUrl& base_url = lxd_socket_url);
    explicit LXDVirtualMachineFactory(NetworkAccessManager::UPtr manager, const Path& data_dir,
                                      const QUrl& base_url = lxd_socket_url);

    VirtualMachine::UPtr create_virtual_machine(const VirtualMachineDescription& desc,
                                                VMStatusMonitor& monitor) override;
    void remove_resources_for(const std::string& name) override;
    FetchType fetch_type() override;
    VMImage prepare_source_image(const VMImage& source_image) override;
    void prepare_instance_image(const VMImage& instance_image, const VirtualMachineDescription& desc) override;
    void configure(const std::string& name, YAML::Node& meta_config, YAML::Node& user_config) override;
    void hypervisor_health_check() override;
    QString get_backend_directory_name() override
    {
        return "lxd";
    };
    QString get_backend_version_string() override
    {
        return "lxd";
    };
    NetworkAccessManager* get_network_manager() const;
    QUrl get_base_url() const;

private:
    NetworkAccessManager::UPtr manager;
    const Path data_dir;
    const QUrl base_url;
};
} // namespace multipass

#endif // MULTIPASS_LXD_VIRTUAL_MACHINE_FACTORY_H
