/*******************************************************************************
 * Copyright 2022 MINRES Technologies GmbH
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 *******************************************************************************/

#include "hierarchy_dumper.h"
#include "configurer.h"
#include "tracer.h"
#include "perf_estimator.h"
#include <fstream>
#include "report.h"
#include <tlm>
#include <unordered_set>
#include <iostream>
#include <fstream>
#include <regex>
#include <sstream>
#include <rapidjson/rapidjson.h>
#include <rapidjson/istreamwrapper.h>
#include <rapidjson/ostreamwrapper.h>
#include <rapidjson/prettywriter.h>
#include <fmt/format.h>
#include <scc/utilities.h>
#if 0
#include <unordered_map>
template<typename K, typename V>
using map_type = std::unordered_map<K, V>;
#else
#include <map>
template<typename K, typename V>
using map_type = std::map<K, V>;
#endif

#include <string>
#include <typeinfo>
#ifdef __GNUG__
#include <cstdlib>
#include <memory>
#include <cxxabi.h>
#endif

namespace scc {
using namespace rapidjson;
using writer_type = PrettyWriter<OStreamWrapper>;

namespace {
#ifdef __GNUG__
std::string demangle(const char* name) {
    int status = -4; // some arbitrary value to eliminate the compiler warning
    // enable c++11 by passing the flag -std=c++11 to g++
    std::unique_ptr<char, void(*)(void*)> res {
        abi::__cxa_demangle(name, NULL, NULL, &status),
                std::free
    };
    return (status==0) ? res.get() : name ;
}
#else
// does nothing if not g++
std::string demangle(const char* name) {
    return name;
}
#endif

template <class T>
std::string type(const T& t) {
    return demangle(typeid(t).name());
}

struct Edge;
struct Port;
struct Module;
unsigned object_counter{0};
std::vector<std::unique_ptr<Port>> ports;
std::vector<std::unique_ptr<Edge>> edges;

struct Port {
    std::string const fullname;
    std::string const name;
    sc_core::sc_interface const* port_if{nullptr};
    bool input{false};
    std::string const type;
    std::string const sig_name;
    std::string const id{fmt::format("{}", ++object_counter)};
    Module* const module;
    std::vector<Edge*> edges;

    static Port* get(std::string const& fullname, std::string const& name, sc_core::sc_interface  const* ptr, bool input, std::string const& type, Module* module, std::string const& sig_name ="") {
        ports.emplace_back(new Port(fullname, name, ptr, input, type, module, sig_name));
        return ports.back().get();
    }
protected:
    Port(std::string const& fullname, std::string const& name, sc_core::sc_interface  const* ptr, bool input, std::string const& type, Module* module, std::string const& sig_name ="")
    : fullname(fullname)
    , name(name)
    , port_if(ptr)
    , input(input)
    , type(type)
    , sig_name(sig_name)
    , module(module)
    { }
};

struct Module {
    std::string const fullname;
    std::string const name;
    std::string const type;
    std::string const id{fmt::format("{}", ++object_counter)};
    std::vector<std::unique_ptr<Module>> submodules;
    Module* const parent;
    std::vector<Port*> ports;
    std::vector<Edge*> edges;

    Module(std::string const& fullname, std::string const& name, std::string const& type, Module* parent)
    : fullname(fullname)
    , name(name)
    , type(type)
    , parent(parent)
    { }
};

struct Edge {
    Port const* srcPort;
    Port const* tgtPort;
    std::string const id{fmt::format("{}", ++object_counter)};
    static Edge* get(Port* srcPort, Port* tgtPort) {
        edges.emplace_back(new Edge(srcPort, tgtPort));
        auto myself = edges.back().get();
        srcPort->edges.push_back(myself);
        tgtPort->edges.push_back(myself);
        return myself;
    }
protected:
    explicit Edge(Port* srcPort, Port* tgtPort): srcPort(srcPort), tgtPort(tgtPort){
        if(id=="1062")
            std::cout<<"Plumberquatsch";
    }
};

const std::unordered_set<std::string> know_entities = {
        "tlm_initiator_socket", "sc_export", "sc_thread_process", "sc_signal",
        "sc_object", "sc_fifo", "sc_method_process", "sc_mutex", "sc_vector",
        "sc_semaphore_ordered", "sc_variable", "sc_prim_channel", "tlm_signal"
};

std::string indent{"    "};
std::string operator* (std::string const& str, const unsigned int level) {
    std::ostringstream ss;
    for (unsigned int i = 0; i < level; i++) ss << str;
    return ss.str();
}

#if TLM_VERSION_MAJOR==2 and TLM_VERSION_MINOR == 0           ///< version minor level ( numeric )
#if TLM_VERSION_PATCH == 6           ///< version patch level ( numeric )
#define GET_EXPORT_IF(tptr) tptr->get_base_export().get_interface()
#define GET_PORT_IF(tptr)   tptr->get_base_port().get_interface()
#elif TLM_VERSION_PATCH == 5
#define GET_EXPORT_IF(tptr) tptr->get_export_base().get_interface()
#define GET_PORT_IF(tptr)   tptr->get_port_base().get_interface()
#else
#define NO_TLM_EXTRACT
#endif
#endif

std::vector<std::string> traverse_hierarchy(sc_core::sc_object const* obj, Module *currentModule, unsigned const level) {
    SCCDEBUG() << indent*level<< obj->name() << "(" << obj->kind() << ")";
    std::string kind{obj->kind()};
    if (kind == "sc_module") {
        if(std::string(obj->basename()).substr(0, 3) == "$$$")
            return {};
        auto* name = obj->name();
        currentModule->submodules.emplace_back(new Module(name, obj->basename(), type(*obj), currentModule));
        std::unordered_set<std::string> keep_outs;
        for (auto* child : obj->get_child_objects()) {
            const std::string child_name{child->basename()};
            if(child_name.substr(0, 3)=="$$$")
                continue;
            if(!keep_outs.empty()) {
                auto it = std::find_if(std::begin(keep_outs), std::end(keep_outs), [&child_name](std::string const& e){
                    return child_name.size() > e.size() && child_name.substr(0, e.size()) == e;
                });
                if(it!=std::end(keep_outs))
                    continue;
            }
            auto ks = traverse_hierarchy(child, currentModule->submodules.back().get(), level+1);
            if(ks.size())
                for(auto& s:ks) keep_outs.insert(s);
        }
    } else if(kind == "sc_clock"){
        auto const* iface = dynamic_cast<sc_core::sc_interface const*>(obj);
        currentModule->submodules.emplace_back(new Module(obj->name(), obj->basename(), type(*obj), currentModule));
        currentModule->submodules.back()->ports.push_back(
                Port::get(std::string(obj->name())+"."+obj->basename(), obj->basename(), iface, false, obj->kind(), currentModule, obj->basename()));
#ifndef NO_TLM_EXTRACT
    } else if(auto const* tptr = dynamic_cast<tlm::tlm_base_socket_if const*>(obj)) {
        auto cat = tptr->get_socket_category();
        bool input = (cat & tlm::TLM_TARGET_SOCKET) == tlm::TLM_TARGET_SOCKET;
        currentModule->ports.push_back(Port::get(obj->name(), obj->basename(), input?GET_EXPORT_IF(tptr):GET_PORT_IF(tptr), input, obj->kind(), currentModule));
        return {
            std::string(obj->basename())+"_port", std::string(obj->basename())+"_export",
                    std::string(obj->basename())+"_port_0", std::string(obj->basename())+"_export_0"
        };
#endif
    } else if (auto const* optr = dynamic_cast<sc_core::sc_port_base const*>(obj)) {
        if(std::string(optr->basename()).substr(0, 3)!="$$$") {
            sc_core::sc_interface const* if_ptr = optr->get_interface();
            sc_core::sc_prim_channel const* if_obj = dynamic_cast<sc_core::sc_prim_channel const*>(if_ptr);
            bool is_input = kind == "sc_in" || kind == "sc_fifo_in";
            currentModule->ports.push_back(
                    Port::get(obj->name(), obj->basename(), if_ptr, is_input, obj->kind(), currentModule, if_obj?if_obj->basename():""));
        }
    } else if (auto const* optr = dynamic_cast<sc_core::sc_export_base const*>(obj)) {
        if(std::string(optr->basename()).substr(0, 3)!="$$$") {
            sc_core::sc_interface const* pointer = optr->get_interface();
            currentModule->ports.push_back(Port::get(obj->name(), obj->basename(), pointer, true, obj->kind(), currentModule));
        }
    } else if (know_entities.find(std::string(obj->kind())) == know_entities.end()) {
        SCCWARN() << "object not known (" << std::string(obj->kind()) << ")";
    }
    return {};
}

void extract_edges(Module* module){
    for (auto& m : module->submodules)
        extract_edges(m.get());
    // Draw edges module <-> submodule:
    for (auto srcport : module->ports)
        if(srcport->port_if)
            for (auto& tgtmod : module->submodules)
                for (auto tgtport : tgtmod->ports)
                    if (tgtport->port_if == srcport->port_if)
                        module->edges.push_back(Edge::get(srcport, tgtport));
    // Draw edges submodule -> submodule:
    for (auto& srcmod : module->submodules)
        for (auto srcport : srcmod->ports)
            if(!srcport->input && srcport->port_if)
                for (auto& tgtmod : module->submodules)
                    for (auto tgtport : tgtmod->ports) {
                        if(srcmod->fullname == tgtmod->fullname && tgtport->fullname == srcport->fullname)
                            continue;
                        if (tgtport->port_if == srcport->port_if && tgtport->input)
                            module->edges.push_back(Edge::get(srcport, tgtport));
                    }
}

void collect_open_ports(Module* module, map_type<sc_core::sc_interface const*, std::vector<Port*>>& port_by_if, map_type<std::string, Module*>& module_by_name){
    module_by_name[module->fullname]=module;
    for (auto& m : module->submodules)
        collect_open_ports(m.get(), port_by_if, module_by_name);
    for(auto p : module->ports) {
        if(p->edges.empty())
            port_by_if[p->port_if].push_back(p);
    }
}

void connect_ports(std::vector<Port*>& ports, map_type<std::string, Module*> module_by_name) {
    std::vector<Port*> tgtports;
    std::vector<Port*> srcports;
    std::copy_if(std::begin(ports), std::end(ports), std::back_inserter(tgtports), [](Port*p ){ return p->input;});
    std::copy_if(std::begin(ports), std::end(ports), std::back_inserter(srcports), [](Port*p ){ return !p->input;});
    if(srcports.size()!=1) return;
    auto srcport = srcports[0];
    auto src_mod = srcport->module;
    auto src_mod_hier = util::split(src_mod->fullname, '.');
    for(auto tgtport:tgtports){
        auto tgt_mod = tgtport->module;
        auto tgt_mod_hier = util::split(tgt_mod->fullname, '.');
        size_t common_depth=0;
        for(; common_depth<std::min(src_mod_hier.size(), tgt_mod_hier.size()) && src_mod_hier[common_depth] == tgt_mod_hier[common_depth]; ++common_depth);
        auto common_mod_hier_name = util::join(std::vector<std::string>(tgt_mod_hier.begin(), tgt_mod_hier.begin() + common_depth), ".");
        auto common_it = module_by_name.find(common_mod_hier_name);
        assert(common_it!=module_by_name.end());
        auto common_mod=common_it->second;
        // infer src port from srcport's module upwards if needed
        auto cur_src_port = srcport;
        if(src_mod!=common_mod) {
            auto cur_parent_mod = src_mod->parent;
            while(cur_parent_mod != common_mod){
                auto it = std::find_if(cur_parent_mod->ports.begin(), cur_parent_mod->ports.end(), [srcport](Port const* p){
                    return srcport->port_if == p->port_if;
                });
                if(it == cur_parent_mod->ports.end()) {

                    cur_parent_mod->ports.push_back(Port::get(cur_parent_mod->fullname+"."+srcport->name, srcport->name,
                            srcport->port_if, srcport->input, srcport->type, cur_parent_mod, srcport->sig_name));
                    cur_parent_mod->edges.push_back(Edge::get(cur_src_port, cur_parent_mod->ports.back()));
                    cur_src_port=cur_parent_mod->ports.back();
                } else {
                    cur_src_port=*it;
                }
                cur_parent_mod=cur_parent_mod->parent;
            }
        }
        // infer tgt ports from tgtport's module upwards if needed
        auto cur_tgt_port = tgtport;
        if(tgt_mod!=common_mod) {
            auto cur_parent_mod = tgt_mod->parent;
            while(cur_parent_mod != common_mod){
                auto it = std::find_if(cur_parent_mod->ports.begin(), cur_parent_mod->ports.end(), [tgtport](Port const* p){
                    return tgtport->port_if == p->port_if;
                });
                if(it == cur_parent_mod->ports.end()) {
                    cur_parent_mod->ports.push_back(Port::get(cur_parent_mod->fullname+"."+tgtport->name, tgtport->name,
                            tgtport->port_if, tgtport->input, tgtport->type, cur_parent_mod, tgtport->sig_name));
                    cur_parent_mod->edges.push_back(Edge::get(cur_tgt_port, cur_parent_mod->ports.back()));
                    cur_tgt_port=cur_parent_mod->ports.back();
                } else {
                    cur_tgt_port=*it;
                }
                cur_parent_mod=cur_parent_mod->parent;
            }
        }
        // connect the topmost port if not already done
        auto it = std::find_if(common_mod->edges.begin(), common_mod->edges.end(), [cur_src_port, cur_tgt_port](Edge const* e){
           return e->srcPort==cur_src_port && e->tgtPort==cur_tgt_port;
        });
        if(it==common_mod->edges.end()){
            common_it->second->edges.push_back(Edge::get(cur_src_port, cur_tgt_port));
        } else {
            cur_src_port->edges.push_back(*it);
            cur_tgt_port->edges.push_back(*it);
        }
    }
}

std::unique_ptr<Module> extract_hierarchy() {
    std::vector<sc_core::sc_object*> obj = sc_core::sc_get_top_level_objects();
    std::vector<sc_core::sc_object*> valid_mods;
    std::copy_if(std::begin(obj), std::end(obj), std::back_inserter(valid_mods), [](sc_core::sc_object* p ){
        return std::string(p->kind()) == "sc_module" && std::string(p->basename()).substr(0, 3) != "$$$";
    });
    std::unique_ptr<Module> ret;
    if (valid_mods.size() == 1) {
        SCCDEBUG() << valid_mods[0]->name() << "(" << valid_mods[0]->kind() << ")";
        ret = scc::make_unique<Module>(valid_mods[0]->name(), valid_mods[0]->basename(), type(*valid_mods[0]), nullptr);
        for (auto *child : valid_mods[0]->get_child_objects())
            traverse_hierarchy(child, ret.get(), 1);
    } else if (valid_mods.size() > 1) {
        SCCDEBUG() << "sc_main ( function sc_main() )";
        ret = scc::make_unique<Module>(Module("sc_main", "sc_main", "sc_main()", nullptr));
        for (auto *child : valid_mods)
            traverse_hierarchy(child, ret.get(), 1);
    }
    extract_edges(ret.get());
    map_type<sc_core::sc_interface const*, std::vector<Port*>> port_by_if;
    map_type<std::string, Module*> module_by_name;
    collect_open_ports(ret.get(), port_by_if, module_by_name);
    for(auto& p:port_by_if) {
        connect_ports(std::get<1>(p), module_by_name);
    }
    return ret;
}

void generateElk(std::ostream& e, Module const* module, unsigned level=0) {
    SCCDEBUG() << module->name;
    unsigned num_in{0}, num_out{0};
    for (auto port : module->ports) if(port->input) num_in++; else num_out++;
    if(!module->ports.size() && !module->submodules.size()) return;
    e << indent*level << "node " << module->name << " {" << "\n";
    level++;
    e << indent*level << "layout [ size: 50, "<<std::max(80U, std::max(num_in, num_out)*20)<<" ]\n";
    e << indent*level << "portConstraints: FIXED_SIDE\n";
    e << indent*level << "label \"" << module->name << "\"\n";

    for (auto port : module->ports) {
        SCCDEBUG() << "    " << port->name << "\n";
        auto side = port->input?"WEST":"EAST";
        e << indent*level << "port " << port->name << " { ^port.side: "<<side<<" label '" << port->name<< "' }\n";
    }

    for (auto& m : module->submodules)
        generateElk(e, m.get(), level);
    // Draw edges module <-> submodule:
    for (auto srcport : module->ports) {
        if(srcport->port_if)
            for (auto& tgtmod : module->submodules) {
                for (auto tgtport : tgtmod->ports) {
                    if (tgtport->port_if == srcport->port_if)
                        e << indent*level << "edge " << srcport->fullname << " -> " << tgtport->fullname << "\n";
                }
            }
    }
    // Draw edges submodule -> submodule:
    for (auto& srcmod : module->submodules) {
        for (auto srcport : srcmod->ports) {
            if(!srcport->input && srcport->port_if)
                for (auto& tgtmod : module->submodules) {
                    for (auto tgtport : tgtmod->ports) {
                        if(srcmod->fullname == tgtmod->fullname && tgtport->fullname == srcport->fullname)
                            continue;
                        if (tgtport->port_if == srcport->port_if && tgtport->input)
                            e << indent*level << "edge " << srcport->fullname << " -> " << tgtport->fullname << "\n";
                    }
                }
        }
    }
    level--;
    e << indent*level << "}\n" << "\n";
}

void generatePortJson(writer_type &writer, hierarchy_dumper::file_type type, const scc::Port &p) {
    writer.StartObject(); {
        writer.Key("id"); writer.String(p.id.c_str());
        if(type != hierarchy_dumper::D3JSON) {
            writer.Key("labels"); writer.StartArray(); {
                writer.StartObject(); {
                    writer.Key("text"); writer.String(p.name.c_str());
                } writer.EndObject();
            } writer.EndArray();
            writer.Key("width"); writer.Uint(6);
            writer.Key("height"); writer.Uint(6);
            writer.Key("layoutOptions"); writer.StartObject(); {
                writer.Key("port.side"); writer.String(p.input?"WEST":"EAST");
            } writer.EndObject();
            if(type == hierarchy_dumper::DBGJSON) {
                writer.Key("type"); writer.String(p.type.c_str());
                writer.Key("input"); writer.Bool(p.input);
                writer.Key("interface"); writer.Uint64(reinterpret_cast<uintptr_t>(p.port_if));
            }
        } else {
            writer.Key("direction"); writer.String(p.input?"INPUT":"OUTPUT");
            writer.Key("hwMeta"); writer.StartObject(); {
                writer.Key("name"); writer.String(p.name.c_str());
                // writer.Key("cssClass"); writer.String("node-style0");
                // writer.Key("cssStyle"); writer.String("fill:red");
                writer.Key("connectedAsParent"); writer.Bool(false);
            } writer.EndObject();
            writer.Key("properties"); writer.StartObject(); {
                writer.Key("side"); writer.String(p.input?"WEST":"EAST");
                // writer.Key("index"); writer.Int(0);
            } writer.EndObject();
            writer.Key("children"); writer.StartArray(); writer.EndArray();
        }
    } writer.EndObject();
}

void generateEdgeJson(writer_type &writer, const scc::Port &srcport, const scc::Port &tgtport) {
    writer.StartObject(); {
        auto edge_name = fmt::format("{}", ++object_counter);
        writer.Key("id"); writer.String(edge_name.c_str());
        writer.Key("sources"); writer.StartArray(); {
            writer.String(srcport.id.c_str());
        } writer.EndArray();
        writer.Key("targets"); writer.StartArray(); {
            writer.String(tgtport.id.c_str());
        }writer.EndArray();
    } writer.EndObject();
}

void generateEdgeD3Json(writer_type &writer, const scc::Edge & edge){
    writer.StartObject(); {
        writer.Key("id"); writer.String(edge.id.c_str());
        writer.Key("source");writer.String(edge.srcPort->module->id.c_str());
        writer.Key("sourcePort");writer.String(edge.srcPort->id.c_str());
        writer.Key("target");writer.String(edge.tgtPort->module->id.c_str());
        writer.Key("targetPort");writer.String(edge.tgtPort->id.c_str());
        writer.Key("hwMeta"); writer.StartObject(); {
            if(edge.srcPort->sig_name.size()) {
                writer.Key("name"); writer.String(edge.srcPort->sig_name.c_str());
            } else if(edge.tgtPort->sig_name.size()) {
                writer.Key("name"); writer.String(edge.tgtPort->sig_name.c_str());
            } else {
                writer.Key("name"); writer.String(fmt::format("[{}->{}]", edge.srcPort->name, edge.tgtPort->name).c_str());
            }
            // writer.Key("cssClass"); writer.String("link-style0");
            // writer.Key("cssStyle"); writer.String("stroke:red");
        } writer.EndObject();
    } writer.EndObject();
}

void generateModJson(writer_type& writer, hierarchy_dumper::file_type type, Module const& module, unsigned level=0) {
    unsigned num_in{0}, num_out{0};
    for (auto port : module.ports) if(port->input) num_in++; else num_out++;
    writer.StartObject(); {
        writer.Key("id"); writer.String(module.id.c_str());
        // process ports
        writer.Key("ports"); writer.StartArray(); {
            for(auto& p: module.ports) generatePortJson(writer, type, *p);
        } writer.EndArray();
        // process modules
        if(type==hierarchy_dumper::D3JSON && !module.parent) writer.Key("_children"); else
            writer.Key("children"); writer.StartArray(); {
            for(auto& c: module.submodules) generateModJson(writer, type, *c, level*1);
        } writer.EndArray();
        // process connections
        if(type == hierarchy_dumper::D3JSON) {
            if(!module.parent) writer.Key("_edges"); else
            writer.Key("edges"); writer.StartArray(); {
                for(auto& e: module.edges) {
                    generateEdgeD3Json(writer, *e);
                }
            } writer.EndArray();
            writer.Key("hwMeta"); writer.StartObject(); {
                writer.Key("name"); writer.String(module.name.c_str());
                writer.Key("cls"); writer.String(module.type.c_str());
                // writer.Key("bodyText"); writer.String(module.type.c_str());
                writer.Key("maxId"); writer.Uint(object_counter);
                writer.Key("isExternalPort"); writer.Bool(false);
                // writer.Key("cssClass"); writer.String("node-style0");
                // writer.Key("cssStyle"); writer.String("fill:red");
            } writer.EndObject();
            writer.Key("properties"); writer.StartObject(); {
                writer.Key("org.eclipse.elk.layered.mergeEdges");  writer.Uint(1);
                writer.Key("org.eclipse.elk.portConstraints"); writer.String("FIXED_SIDE");
            } writer.EndObject();
        } else {
            writer.Key("edges"); writer.StartArray(); {
                for(auto& e: module.edges) {
                    generateEdgeJson(writer, *e->srcPort, *e->tgtPort);
                }
            } writer.EndArray();
            writer.Key("labels"); writer.StartArray(); {
                writer.StartObject(); {
                    writer.Key("text"); writer.String(module.name.c_str());
                } writer.EndObject();
            } writer.EndArray();
            writer.Key("width"); writer.Uint(50);
            writer.Key("height"); writer.Uint(std::max(80U, std::max(num_in, num_out)*20));
            if(type == hierarchy_dumper::DBGJSON) {
                writer.Key("name"); writer.String(module.name.c_str());
                writer.Key("type"); writer.String(module.type.c_str());
                writer.Key("topmodule"); writer.Bool(module.parent);
            }
        }
    } writer.EndObject();
}

void dump_structure(std::ostream& e, hierarchy_dumper::file_type format) {
    auto top_mod = extract_hierarchy();
    if(format == hierarchy_dumper::ELKT) {
        e<<"algorithm: org.eclipse.elk.layered\n";
        e<<"edgeRouting: ORTHOGONAL\n";
        generateElk(e, top_mod.get());
        SCCINFO() << "SystemC Structure Dumped to ELK file";
    } else {
        OStreamWrapper stream(e);
        writer_type writer(stream);
        writer.StartObject(); {
            auto elems = util::split(sc_core::sc_argv()[0], '/');
            writer.Key("id"); writer.String("0");
            writer.Key("labels"); writer.StartArray(); {
                writer.StartObject(); {
                    writer.Key("text"); writer.String(elems[elems.size()-1].c_str());
                } writer.EndObject();
            } writer.EndArray();
            writer.Key("layoutOptions"); writer.StartObject(); {
                writer.Key("algorithm"); writer.String("layered");
            } writer.EndObject();
            writer.Key("children"); writer.StartArray(); {
                generateModJson(writer, format, *top_mod.get());
            } writer.EndArray();
            writer.Key("edges");writer.StartArray();
            writer.EndArray();
            if(format == hierarchy_dumper::D3JSON) {
                writer.Key("hwMeta"); writer.StartObject(); {
                    writer.Key("cls"); writer.Null();
                    writer.Key("maxId"); writer.Uint(65536);
                    writer.Key("name"); writer.String(elems[elems.size()-1].c_str());
                } writer.EndObject();
                writer.Key("properties"); writer.StartObject(); {
                    writer.Key("org.eclipse.elk.layered.mergeEdges");  writer.Uint(1);
                    writer.Key("org.eclipse.elk.portConstraints"); writer.String("FIXED_ORDER");
                } writer.EndObject();
            }
        } writer.EndObject();
        SCCINFO() << "SystemC Structure Dumped to JSON file";
    }
}
} // namespace anonymous

hierarchy_dumper::hierarchy_dumper(const std::string &filename, file_type format)
: sc_core::sc_module(sc_core::sc_module_name("$$$hierarchy_dumper$$$"))
, dump_hier_file_name{filename}
, dump_format{format}
{
}

hierarchy_dumper::~hierarchy_dumper() {
}

void hierarchy_dumper::start_of_simulation() {
    if(dump_hier_file_name.size()) {
        std::ofstream of{dump_hier_file_name};
        if(of.is_open())
            dump_structure(of, dump_format);
    }
}
}
