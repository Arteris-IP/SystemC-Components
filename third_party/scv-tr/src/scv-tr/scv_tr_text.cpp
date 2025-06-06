//  -*- C++ -*- <this line is for emacs to recognize it as C++ code>
/*****************************************************************************
 Licensed to Accellera Systems Initiative Inc. (Accellera)
 under one or more contributor license agreements.  See the
 NOTICE file distributed with this work for additional
 information regarding copyright ownership. Accellera licenses
 this file to you under the Apache License, Version 2.0 (the
 "License"); you may not use this file except in compliance
 with the License.  You may obtain a copy of the License at
 http://www.apache.org/licenses/LICENSE-2.0
 Unless required by applicable law or agreed to in writing,
 software distributed under the License is distributed on an
 "AS IS" BASIS, WITHOUT WARRANTIES OR CONDITIONS OF ANY
 KIND, either express or implied.  See the License for the
 specific language governing permissions and limitations
 under the License.
 *****************************************************************************/
/*****************************************************************************
 scv_tr_text.cpp -- This is the implementation of the transaction recording
 text facility, which uses callbacks in scv_tr.h
 Original Authors (Cadence Design Systems, Inc):
 Norris Ip, Dean Shea, John Rose, Jasvinder Singh, William Paulsen,
 John Pierce, Rachida Kebichi, Ted Elkind, David Bailey
 2002-09-23
 *****************************************************************************/
/*****************************************************************************
 MODIFICATION LOG - modifiers, enter your name, affiliation, date and
 changes you are making here.
 Name, Affiliation, Date: Torsten Maehne,
 Universite Pierre et Marie Curie, 2013-04-30
 Description of Modification: Fix memory leak caused by not free()-ing
 C-strings duplicated using strdup() in the
 function scv_tr_db_cbf() by changing the type
 of the concerned static variable
 my_text_file_namefrom char* to string.
 Remove the now unused internal
 static scv_tr_strdup() function, which is anyway
 not exported by the linker.
 *****************************************************************************/
/*
 * Here's the format of the text file:
 scv_tr_stream (ID <id>, name "<full_name>", kind "<kind>")
 scv_tr_generator (ID <id>, name "<name>", scv_tr_stream <id>,
 begin_attribute (ID <id1>, name "<name1>", type <"type_name">)
 begin_attribute (ID <id2>, name "<name2>", type <"type_name">)
 end_attribute (ID <id3>, name "<name3>", type <"type_name">)
 end_attribute (ID <id4>, name "<name4>", type <"type_name">)
 ... )
 tx_begin <this_transaction_id> <generator_id> <begin_time>
 a <value>
 a <value>
 tx_end <this_transaction_id> <generator_id> <end_time>
 a <value>
 a <value>
 tx_relation <"relation_name"> <tx_id_1> <tx_id_2>
 *
 */
#include "scv_introspection.h"
#include "scv_tr.h"
#include <array>
#include <cerrno>
#include <cstring>
#include <fmt/format.h>
#include <fmt/os.h>
#include <fmt/printf.h>
#include <sstream>
#include <string>

namespace scv_tr {
// ----------------------------------------------------------------------------
// template <typename... Args>
// inline void scv_tr_fprintf(FILE* f, char const* fmt_str, const Args&... args){
//    auto buf = fmt::format(FMT_STRING(fmt_str), args...);
//    std::fwrite(buf.c_str(), 1, buf.size(), f);
//}
//#define FPRINF(...) fmt::fprintf(__VA_ARGS__)
#define FPRINT(FP, FMTSTR)                                                                                             \
    auto buf1 = fmt::format(FMT_STRING(FMTSTR));                                                                       \
    std::fwrite(buf1.c_str(), 1, buf1.size(), FP);
#define FPRINTF(FP, FMTSTR, ...)                                                                                       \
    auto buf2 = fmt::format(FMT_STRING(FMTSTR), __VA_ARGS__);                                                          \
    std::fwrite(buf2.c_str(), 1, buf2.size(), FP);
// ----------------------------------------------------------------------------
static FILE* my_text_file_p = nullptr;
static void scv_tr_db_cbf(const scv_tr_db& _scv_tr_db, scv_tr_db::callback_reason reason, void* user_data_p) {
    // This is called from the scv_tr_db ctor.
    static std::string my_text_file_name("DEFAULT_scv_tr_TEXT.txt");
    switch(reason) {
    case scv_tr_db::CREATE:
        if((_scv_tr_db.get_name() != nullptr) && (strlen(_scv_tr_db.get_name()) != 0)) {
            my_text_file_name = _scv_tr_db.get_name();
        }
        my_text_file_p = fopen(my_text_file_name.c_str(), "w");
        if(my_text_file_p == nullptr) {
            std::stringstream ss;
            ss << "Can't open text recording file. " << strerror(errno);
            _scv_message::message(_scv_message::TRANSACTION_RECORDING_INTERNAL, ss.str().c_str());
        } else {
            std::stringstream ss;
            ss << "opening file " << my_text_file_name;
            _scv_message::message(_scv_message::TRANSACTION_RECORDING_INTERNAL_INFO, ss.str().c_str());
        }
        break;
    case scv_tr_db::DELETE:
        if(my_text_file_p != nullptr) {
            std::stringstream ss;
            ss << "closing file " << my_text_file_name;
            _scv_message::message(_scv_message::TRANSACTION_RECORDING_INTERNAL_INFO, ss.str().c_str());
            fclose(my_text_file_p);
            my_text_file_p = nullptr;
        }
        break;
    default:
        _scv_message::message(_scv_message::TRANSACTION_RECORDING_INTERNAL, "Unknown reason in scv_tr_db callback");
    }
}
// ----------------------------------------------------------------------------
static void scv_tr_stream_cbf(const scv_tr_stream& s, scv_tr_stream::callback_reason reason, void* user_data_p) {
    if(reason == scv_tr_stream::CREATE) {
        if(my_text_file_p == nullptr)
            return;
        FPRINTF(my_text_file_p, "scv_tr_stream (ID {}, name \"{}\", kind \"{}\")\n", s.get_id(), s.get_name(),
                s.get_stream_kind() ? s.get_stream_kind() : "<no_stream_kind>");
    }
}
// ----------------------------------------------------------------------------
static void do_attributes(bool declare_attributes, // If false then print the values
                          bool undefined_values, bool is_record_attribute, std::string& prefix_name,
                          const std::string& exts_kind, const scv_extensions_if* my_exts_p,
                          int* index) // The attribute index number
{
    // This function can be called recursively, for nested data types.
    if(my_exts_p == nullptr)
        return;
    std::string full_name;
    if(prefix_name == "") {
        full_name = my_exts_p->get_name();
    } else {
        if((my_exts_p->get_name() == nullptr) || (strlen(my_exts_p->get_name()) == 0)) {
            full_name = prefix_name;
        } else {
            full_name = prefix_name + "." + my_exts_p->get_name();
        }
    }
    if(full_name == "") {
        full_name = "<anonymous>";
    }
    switch(my_exts_p->get_type()) {
    case scv_extensions_if::RECORD: {
        int num_fields = my_exts_p->get_num_fields();
        int field_counter;
        if(num_fields > 0) {
            for(field_counter = 0; field_counter < num_fields; field_counter++) {
                const scv_extensions_if* field_data_p = my_exts_p->get_field(field_counter);
                do_attributes(declare_attributes, undefined_values, is_record_attribute, prefix_name, exts_kind,
                              field_data_p, index);
            }
        }
    } break;
    case scv_extensions_if::ENUMERATION: {
        if(declare_attributes) {
            FPRINTF(my_text_file_p, "{} (ID {}, name \"{}\", type \"ENUMERATION\")\n",
                    exts_kind, // begin_attribute or end_attribute
                    *index, full_name);
            (*index)++;
        } else if(undefined_values) {
            FPRINT(my_text_file_p, "a UNDEFINED\n");
        } else {
            if(is_record_attribute) {
                FPRINTF(my_text_file_p, "{} \"{}\" ENUMERATION = \"{}\"\n", exts_kind, full_name,
                        my_exts_p->get_enum_string((int)my_exts_p->get_integer()));
            } else {
                FPRINTF(my_text_file_p, "a \"{}\"\n", my_exts_p->get_enum_string((int)my_exts_p->get_integer()));
            }
        }
    } break;
    case scv_extensions_if::BOOLEAN: {
        if(declare_attributes) {
            FPRINTF(my_text_file_p, "{} (ID {}, name \"{}\", type \"BOOLEAN\")\n",
                    exts_kind, // begin_attribute or end_attribute
                    *index, full_name);
            (*index)++;
        } else if(undefined_values) {
            FPRINT(my_text_file_p, "a UNDEFINED\n");
        } else {
            if(is_record_attribute) {
                FPRINTF(my_text_file_p, "{} \"{}\" BOOLEAN = {}\n", exts_kind, full_name,
                        my_exts_p->get_bool() ? "true" : "false");
            } else {
                FPRINTF(my_text_file_p, "a {}\n", my_exts_p->get_bool() ? "true" : "false");
            }
        }
    } break;
    case scv_extensions_if::INTEGER:
    case scv_extensions_if::FIXED_POINT_INTEGER: {
        if(declare_attributes) {
            FPRINTF(my_text_file_p, "{} (ID {}, name \"{}\", type \"INTEGER\")\n",
                    exts_kind, // begin_attribute or end_attribute
                    *index, full_name);
            (*index)++;
        } else if(undefined_values) {
            FPRINT(my_text_file_p, "a UNDEFINED\n");
        } else {
            if(is_record_attribute) {
                FPRINTF(my_text_file_p, "{} \"{}\" INTEGER = {}\n", exts_kind, full_name, my_exts_p->get_integer());
            } else {
                FPRINTF(my_text_file_p, "a {}\n", my_exts_p->get_integer());
            }
        }
    } break;
    case scv_extensions_if::UNSIGNED: {
        if(declare_attributes) {
            FPRINTF(my_text_file_p, "{} (ID {}, name \"{}\", type \"UNSIGNED\")\n",
                    exts_kind, // begin_attribute or end_attribute
                    *index, full_name);
            (*index)++;
        } else if(undefined_values) {
            FPRINT(my_text_file_p, "a UNDEFINED\n");
        } else {
            if(is_record_attribute) {
                FPRINTF(my_text_file_p, "{} \"{}\" UNSIGNED = {}\n", exts_kind, full_name, my_exts_p->get_unsigned());
            } else {
                FPRINTF(my_text_file_p, "a {}\n", my_exts_p->get_unsigned());
            }
        }
    } break;
    case scv_extensions_if::POINTER: {
        const scv_extensions_if* field_data_p = my_exts_p->get_pointer();
        // Extensions are not yet implemented for pointers, so the only thing
        // to do here is to simply print the value of the pointer.
        if(declare_attributes) {
            FPRINTF(my_text_file_p, "{} (ID {}, name \"{}\", type \"POINTER\")\n",
                    exts_kind, // begin_attribute or end_attribute
                    *index, full_name);
            (*index)++;
        } else if(undefined_values) {
            FPRINT(my_text_file_p, "a UNDEFINED\n");
        } else {
            if(is_record_attribute) {
                FPRINTF(my_text_file_p, "{} \"{}\" POINTER = {}\n", exts_kind, full_name, (void*)field_data_p);
            } else {
                FPRINTF(my_text_file_p, "a {}\n", (void*)field_data_p);
            }
        }
    } break;
    case scv_extensions_if::STRING: {
        if(declare_attributes) {
            FPRINTF(my_text_file_p, "{} (ID {}, name \"{}\", type \"STRING\")\n",
                    exts_kind, // begin_attribute or end_attribute
                    *index, full_name);
            (*index)++;
        } else if(undefined_values) {
            FPRINT(my_text_file_p, "a UNDEFINED\n");
        } else {
            if(is_record_attribute) {
                FPRINTF(my_text_file_p, "{} \"{}\" STRING = \"{}\"\n", exts_kind, full_name, my_exts_p->get_string());
            } else {
                FPRINTF(my_text_file_p, "a \"{}\"\n", my_exts_p->get_string());
            }
        }
    } break;
    case scv_extensions_if::FLOATING_POINT_NUMBER: {
        if(declare_attributes) {
            FPRINTF(my_text_file_p, "{} (ID {}, name \"{}\", type \"FLOATING_POINT_NUMBER\")\n",
                    exts_kind, // begin_attribute or end_attribute
                    *index, full_name);
            (*index)++;
        } else if(undefined_values) {
            FPRINT(my_text_file_p, "a UNDEFINED\n");
        } else {
            if(is_record_attribute) {
                FPRINTF(my_text_file_p, "{} \"{}\" FLOATING_POINT_NUMBER = {}\n", exts_kind, full_name,
                        my_exts_p->get_double());
            } else {
                FPRINTF(my_text_file_p, "a {}\n", my_exts_p->get_double());
            }
        }
    } break;
    case scv_extensions_if::BIT_VECTOR: {
        if(declare_attributes) {
            FPRINTF(my_text_file_p, "{} (ID {}, name \"{}\", type \"BIT_VECTOR[{}]\")\n",
                    exts_kind, // begin_attribute or end_attribute
                    *index, full_name, my_exts_p->get_bitwidth());
            (*index)++;
        } else if(undefined_values) {
            FPRINT(my_text_file_p, "a UNDEFINED\n");
        } else {
            sc_bv_base tmp_bv(my_exts_p->get_bitwidth());
            my_exts_p->get_value(tmp_bv);
            if(is_record_attribute) {
                FPRINTF(my_text_file_p, "{} \"{}\" BIT_VECTOR = \"{}\"\n", exts_kind, full_name, tmp_bv.to_string());
            } else {
                FPRINTF(my_text_file_p, "a \"{}\"\n", tmp_bv.to_string());
            }
        }
    } break;
    case scv_extensions_if::LOGIC_VECTOR: {
        if(declare_attributes) {
            FPRINTF(my_text_file_p, "{} (ID {}, name \"{}\", type \"LOGIC_VECTOR[{}]\")\n",
                    exts_kind, // begin_attribute or end_attribute
                    *index, full_name, my_exts_p->get_bitwidth());
            (*index)++;
        } else if(undefined_values) {
            FPRINT(my_text_file_p, "a UNDEFINED\n");
        } else {
            sc_lv_base tmp_lv(my_exts_p->get_bitwidth());
            my_exts_p->get_value(tmp_lv);
            if(is_record_attribute) {
                FPRINTF(my_text_file_p, "{} \"{}\" LOGIC_VECTOR = \"{}\"\n", exts_kind, full_name, tmp_lv.to_string());
            } else {
                FPRINTF(my_text_file_p, "a \"{}\"\n", tmp_lv.to_string());
            }
        }
    } break;
    case scv_extensions_if::ARRAY: {
        int array_elt_index = 0;
        for(; array_elt_index < my_exts_p->get_array_size(); array_elt_index++) {
            const scv_extensions_if* field_data_p = my_exts_p->get_array_elt(array_elt_index);
            do_attributes(declare_attributes, undefined_values, is_record_attribute, prefix_name, exts_kind,
                          field_data_p, index);
        }
    } break;
    default: {
        std::string s = fmt::format(FMT_STRING("Unsupported attribute type = {0}."), static_cast<int>(my_exts_p->get_type()));
        _scv_message::message(_scv_message::TRANSACTION_RECORDING_INTERNAL, s.c_str());
    }
    }
}
// ----------------------------------------------------------------------------
static void scv_tr_generator_cbf(const scv_tr_generator_base& g, scv_tr_generator_base::callback_reason reason,
                                 void* user_data_p) {
    if(reason != scv_tr_generator_base::CREATE) {
        return;
    }
    if(my_text_file_p == nullptr)
        return;
    FPRINTF(my_text_file_p, "scv_tr_generator (ID {}, name \"{}\", scv_tr_stream {},\n", g.get_id(), g.get_name(),
            g.get_scv_tr_stream().get_id());
    std::string exts_kind;
    int index = 0;
    const scv_extensions_if* my_begin_exts_p = g.get_begin_exts_p();
    if(my_begin_exts_p != nullptr) {
        exts_kind = "begin_attribute";
        std::string tmp_str = g.get_begin_attribute_name() ? g.get_begin_attribute_name() : "";
        do_attributes(true, false, false, tmp_str, exts_kind, my_begin_exts_p, &index);
    }
    const scv_extensions_if* my_end_exts_p = g.get_end_exts_p();
    if(my_end_exts_p != nullptr) {
        exts_kind = "end_attribute";
        std::string tmp_str = g.get_end_attribute_name() ? g.get_end_attribute_name() : "";
        do_attributes(true, false, false, tmp_str, exts_kind, my_end_exts_p, &index);
    }
    FPRINT(my_text_file_p, ")\n");
}
// ----------------------------------------------------------------------------
static void scv_tr_handle_cbf(const scv_tr_handle& t, scv_tr_handle::callback_reason reason, void* user_data_p) {
    if(my_text_file_p == nullptr)
        return;
    int i = 0;
    // This callback function is called when a transaction is begun or ended,
    // or deleted.
    // First check to be sure transaction recording is enabled:
    //
    if(t.get_scv_tr_stream().get_scv_tr_db() == nullptr)
        return;
    if(t.get_scv_tr_stream().get_scv_tr_db()->get_recording() == false)
        return;
    const scv_extensions_if* my_exts_p;
    switch(reason) {
    case scv_tr_handle::BEGIN: {
        // The beginning of a transaction
        FPRINTF(my_text_file_p, "tx_begin {} {} {}\n", t.get_id(), t.get_scv_tr_generator_base().get_id(),
                t.get_begin_sc_time().to_string());
        my_exts_p = t.get_begin_exts_p();
        std::string exts_kind = "begin_attributes";
        bool default_values = false;
        if(my_exts_p == nullptr) {
            // For this transaction, the default attributes are used.
            my_exts_p = t.get_scv_tr_generator_base().get_begin_exts_p();
            default_values = true;
        }
        std::string tmp_str = t.get_scv_tr_generator_base().get_begin_attribute_name()
                                  ? t.get_scv_tr_generator_base().get_begin_attribute_name()
                                  : "";
        do_attributes(false, default_values, false, tmp_str, exts_kind, my_exts_p, &i);
    } break;
    case scv_tr_handle::END: {
        // The end of a transaction
        FPRINTF(my_text_file_p, "tx_end {} {} {}\n", t.get_id(), t.get_scv_tr_generator_base().get_id(),
                t.get_end_sc_time().to_string());
        my_exts_p = t.get_end_exts_p();
        std::string exts_kind = "end_attributes";
        bool default_values = false;
        if(my_exts_p == nullptr) {
            // For this transaction, the default attributes are used.
            my_exts_p = t.get_scv_tr_generator_base().get_end_exts_p();
            default_values = true;
        }
        std::string tmp_str = t.get_scv_tr_generator_base().get_end_attribute_name()
                                  ? t.get_scv_tr_generator_base().get_end_attribute_name()
                                  : "";
        do_attributes(false, default_values, false, tmp_str, exts_kind, my_exts_p, &i);
    } break;
    default:;
    }
}
// ----------------------------------------------------------------------------
static void scv_tr_handle_record_attribute_cbf(const scv_tr_handle& t, const char* attribute_name,
                                               const scv_extensions_if* my_exts_p, void* user_data_p) {
    // First check to be sure transaction recording is enabled:
    //
    if(t.get_scv_tr_stream().get_scv_tr_db() == nullptr)
        return;
    if(t.get_scv_tr_stream().get_scv_tr_db()->get_recording() == false)
        return;
    if(my_text_file_p == nullptr)
        return;
    std::string tmp_str = attribute_name == nullptr ? "" : attribute_name;
    std::string exts_kind = fmt::format(FMT_STRING("tx_record_attribute {0}"), t.get_id());
    do_attributes(false, false, true, tmp_str, exts_kind, my_exts_p, nullptr);
}
// ----------------------------------------------------------------------------
static void scv_tr_handle_relation_cbf(const scv_tr_handle& tr_1, const scv_tr_handle& tr_2, void* user_data_p,
                                       scv_tr_relation_handle_t relation_handle) {
    // First check to be sure transaction recording is enabled:
    //
    if(tr_1.get_scv_tr_stream().get_scv_tr_db() == nullptr)
        return;
    if(tr_1.get_scv_tr_stream().get_scv_tr_db()->get_recording() == false)
        return;
    if(my_text_file_p == nullptr)
        return;
    if(my_text_file_p) {
        FPRINTF(my_text_file_p, "tx_relation \"{}\" {} {}\n",
                tr_1.get_scv_tr_stream().get_scv_tr_db()->get_relation_name(relation_handle), tr_1.get_id(),
                tr_2.get_id());
    }
}
// ----------------------------------------------------------------------------
// ----------------------------------------------------------------------------
void scv_tr_text_init() {
    scv_tr_db::register_class_cb(scv_tr_db_cbf);
    scv_tr_stream::register_class_cb(scv_tr_stream_cbf);
    scv_tr_generator_base::register_class_cb(scv_tr_generator_cbf);
    scv_tr_handle::register_class_cb(scv_tr_handle_cbf);
    scv_tr_handle::register_record_attribute_cb(scv_tr_handle_record_attribute_cbf);
    scv_tr_handle::register_relation_cb(scv_tr_handle_relation_cbf);
}
} // namespace scv_tr
// ----------------------------------------------------------------------------
// ----------------------------------------------------------------------------
