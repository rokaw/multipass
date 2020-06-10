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

#include "iptables_config.h"

#include <multipass/format.h>
#include <multipass/logging/log.h>
#include <multipass/process/process.h>
#include <shared/linux/process_factory.h>

namespace mp = multipass;
namespace mpl = multipass::logging;

namespace
{
// QString constants for all of the different iptables calls
const QString iptables{QStringLiteral("iptables")};
const QString negate{QStringLiteral("!")};

//   Different tables to use
const QString filter{QStringLiteral("filter")};
const QString nat{QStringLiteral("nat")};
const QString mangle{QStringLiteral("mangle")};

//   Chain constants
const QString INPUT{QStringLiteral("INPUT")};
const QString OUTPUT{QStringLiteral("OUTPUT")};
const QString POSTROUTING{QStringLiteral("POSTROUTING")};
const QString FORWARD{QStringLiteral("FORWARD")};

//   option constants
const QString destination{QStringLiteral("--destination")};
const QString delete_rule{QStringLiteral("--delete")};
const QString in_interface{QStringLiteral("--in-interface")};
const QString append_rule{QStringLiteral("--append")};
const QString insert_rule{QStringLiteral("--insert")};
const QString jump{QStringLiteral("--jump")};
const QString match{QStringLiteral("--match")};
const QString out_interface{QStringLiteral("--out-interface")};
const QString protocol{QStringLiteral("--protocol")};
const QString source{QStringLiteral("--source")};
const QString list_rules{QStringLiteral("--list-rules")};
const QString dash_t{QStringLiteral("-t")}; // Use short option for specifying table to avoid var conflicts
const QString wait{QStringLiteral("--wait")};

//   protocol constants
const QString udp{QStringLiteral("udp")};
const QString tcp{QStringLiteral("tcp")};

//   port options and constants
const QString dport{QStringLiteral("--dport")};
const QString sport{QStringLiteral("--sport")};
const QString to_ports{QStringLiteral("--to-ports")};
const QString port_53{QStringLiteral("53")};
const QString port_67{QStringLiteral("67")};
const QString port_68{QStringLiteral("68")};
const QString port_range{QStringLiteral("1024-65535")};

//   rule target constants
const QString ACCEPT{QStringLiteral("ACCEPT")};
const QString MASQUERADE{QStringLiteral("MASQUERADE")};
const QString REJECT{QStringLiteral("REJECT")};
const QString RETURN{QStringLiteral("RETURN")};

//   reject rule constants
const QString reject_with{QStringLiteral("--reject-with")};
const QString icmp_port_unreachable{QStringLiteral("icmp-port-unreachable")};

auto multipass_iptables_comment(const QString& bridge_name)
{
    return QString("generated for Multipass network %1").arg(bridge_name);
}

void add_iptables_rule(const QString& table, const QString& chain, const QStringList& rule, bool append = false)
{
    auto process = mp::ProcessFactory::instance().create_process(
        iptables, QStringList() << wait << dash_t << table << (append ? append_rule : insert_rule) << chain << rule);

    auto exit_state = process->execute();

    if (!exit_state.completed_successfully())
        throw std::runtime_error(
            fmt::format("Failed to set iptables rule for table {}: {}", table, process->read_all_standard_error()));
}

void delete_iptables_rule(const QString& table, const QStringList& chain_and_rule)
{
    auto args = QStringList() << iptables << wait << dash_t << table << delete_rule << chain_and_rule;

    auto process = mp::ProcessFactory::instance().create_process(
        QStringLiteral("sh"), QStringList() << QStringLiteral("-c") << args.join(" "));

    auto exit_state = process->execute();

    if (!exit_state.completed_successfully())
        throw std::runtime_error(
            fmt::format("Failed to delete iptables rule for table {}: {}", table, process->read_all_standard_error()));
}

auto get_iptables_rules(const QString& table)
{
    auto process =
        mp::ProcessFactory::instance().create_process(iptables, QStringList() << wait << dash_t << table << list_rules);

    auto exit_state = process->execute();

    if (!exit_state.completed_successfully())
        throw std::runtime_error(
            fmt::format("Failed to get iptables list for table {}: {}", table, process->read_all_standard_error()));

    return process->read_all_standard_output();
}

void set_iptables_rules(const QString& bridge_name, const QString& cidr, const QString& comment)
{
    const QStringList comment_option{match, QStringLiteral("comment"), QStringLiteral("--comment"), comment};

    // Setup basic iptables overrides for DHCP/DNS
    add_iptables_rule(filter, INPUT,
                      QStringList() << in_interface << bridge_name << protocol << udp << dport << port_67 << jump
                                    << ACCEPT << comment_option);

    add_iptables_rule(filter, INPUT,
                      QStringList() << in_interface << bridge_name << protocol << udp << dport << port_53 << jump
                                    << ACCEPT << comment_option);

    add_iptables_rule(filter, INPUT,
                      QStringList() << in_interface << bridge_name << protocol << tcp << dport << port_53 << jump
                                    << ACCEPT << comment_option);

    add_iptables_rule(filter, OUTPUT,
                      QStringList() << out_interface << bridge_name << protocol << udp << sport << port_67 << jump
                                    << ACCEPT << comment_option);

    add_iptables_rule(filter, OUTPUT,
                      QStringList() << out_interface << bridge_name << protocol << udp << sport << port_53 << jump
                                    << ACCEPT << comment_option);

    add_iptables_rule(filter, OUTPUT,
                      QStringList() << out_interface << bridge_name << protocol << tcp << sport << port_53 << jump
                                    << ACCEPT << comment_option);

    add_iptables_rule(mangle, POSTROUTING,
                      QStringList() << out_interface << bridge_name << protocol << udp << dport << port_68 << jump
                                    << QStringLiteral("CHECKSUM") << QStringLiteral("--checksum-fill")
                                    << comment_option);

    // Do not masquerade to these reserved address blocks.
    add_iptables_rule(nat, POSTROUTING,
                      QStringList() << source << cidr << destination << QStringLiteral("224.0.0.0/24") << jump << RETURN
                                    << comment_option);

    add_iptables_rule(nat, POSTROUTING,
                      QStringList() << source << cidr << destination << QStringLiteral("255.255.255.255/32") << jump
                                    << RETURN << comment_option);

    // Masquerade all packets going from VMs to the LAN/Internet
    add_iptables_rule(nat, POSTROUTING,
                      QStringList() << source << cidr << negate << destination << cidr << protocol << tcp << jump
                                    << MASQUERADE << to_ports << port_range << comment_option);

    add_iptables_rule(nat, POSTROUTING,
                      QStringList() << source << cidr << negate << destination << cidr << protocol << udp << jump
                                    << MASQUERADE << to_ports << port_range << comment_option);

    add_iptables_rule(nat, POSTROUTING,
                      QStringList() << source << cidr << negate << destination << cidr << jump << MASQUERADE
                                    << comment_option);

    // Allow established traffic to the private subnet
    add_iptables_rule(filter, FORWARD,
                      QStringList() << destination << cidr << out_interface << bridge_name << match
                                    << QStringLiteral("conntrack") << QStringLiteral("--ctstate")
                                    << QStringLiteral("RELATED,ESTABLISHED") << jump << ACCEPT << comment_option);

    // Allow outbound traffic from the private subnet
    add_iptables_rule(filter, FORWARD,
                      QStringList() << source << cidr << in_interface << bridge_name << jump << ACCEPT
                                    << comment_option);

    // Allow traffic between virtual machines
    add_iptables_rule(filter, FORWARD,
                      QStringList() << in_interface << bridge_name << out_interface << bridge_name << jump << ACCEPT
                                    << comment_option);

    // Reject everything else
    add_iptables_rule(filter, FORWARD,
                      QStringList() << in_interface << bridge_name << jump << REJECT << reject_with
                                    << icmp_port_unreachable << comment_option,
                      /*append=*/true);

    add_iptables_rule(filter, FORWARD,
                      QStringList() << out_interface << bridge_name << jump << REJECT << reject_with
                                    << icmp_port_unreachable << comment_option,
                      /*append=*/true);
}

void clear_iptables_rules_for(const QString& table, const QString& bridge_name, const QString& cidr,
                              const QString& comment)
{
    auto rules = QString::fromUtf8(get_iptables_rules(table));

    for (auto& rule : rules.split('\n'))
    {
        if (rule.contains(comment) || rule.contains(bridge_name) || rule.contains(cidr))
        {
            // Remove the policy type since delete doesn't use that
            rule.remove(0, 3);

            // Pass the chain and rule wholesale since we capture the whole line
            delete_iptables_rule(table, QStringList() << rule);
        }
    }
}
} // namespace

mp::IPTablesConfig::IPTablesConfig(const QString& bridge_name, const std::string& subnet)
    : bridge_name{bridge_name},
      cidr{QString("%1.0/24").arg(QString::fromStdString(subnet))},
      comment{multipass_iptables_comment(bridge_name)}
{
    try
    {
        clear_all_iptables_rules();
        set_iptables_rules(bridge_name, cidr, comment);
    }
    catch (const std::exception& e)
    {
        mpl::log(mpl::Level::warning, "iptables", e.what());
        iptables_error = true;
        error_string = e.what();
    }
}

mp::IPTablesConfig::~IPTablesConfig()
{
    try
    {
        clear_all_iptables_rules();
    }
    catch (const std::exception& e)
    {
        mpl::log(mpl::Level::warning, "iptables", e.what());
    }
}

void mp::IPTablesConfig::verify_iptables_rules()
{
    if (iptables_error)
    {
        throw std::runtime_error(error_string);
    }
}

void mp::IPTablesConfig::clear_all_iptables_rules()
{
    clear_iptables_rules_for(filter, bridge_name, cidr, comment);
    clear_iptables_rules_for(nat, bridge_name, cidr, comment);
    clear_iptables_rules_for(mangle, bridge_name, cidr, comment);
}
