/***************************************************************************************************
 * Copyright (c) 2012-2019, NVIDIA CORPORATION. All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 *  * Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 *  * Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 *  * Neither the name of NVIDIA CORPORATION nor the names of its
 *    contributors may be used to endorse or promote products derived
 *    from this software without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS ``AS IS'' AND ANY
 * EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR
 * PURPOSE ARE DISCLAIMED.  IN NO EVENT SHALL THE COPYRIGHT OWNER OR
 * CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL,
 * EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO,
 * PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR
 * PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY
 * OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
 * OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 **************************************************************************************************/

#include "pch.h"

#include "i_mdl_elements_function_call.h"

#include "i_mdl_elements_expression.h"
#include "i_mdl_elements_function_definition.h"
#include "i_mdl_elements_module.h"
#include "i_mdl_elements_type.h"
#include "i_mdl_elements_utilities.h"
#include "i_mdl_elements_value.h"
#include "mdl_elements_utilities.h"

#include <sstream>
#include <mi/base/handle.h>
#include <mi/mdl/mdl_mdl.h>
#include <mi/mdl/mdl_symbols.h>
#include <mi/mdl/mdl_code_generators.h>
#include <mi/neuraylib/istring.h>
#include <base/system/main/access_module.h>
#include <base/lib/log/i_log_logger.h>
#include <base/lib/config/config.h>
#include <base/data/db/i_db_access.h>
#include <base/data/serial/i_serializer.h>
#include <base/util/registry/i_config_registry.h>
#include <io/scene/scene/i_scene_journal_types.h>
#include <mdl/integration/mdlnr/i_mdlnr.h>

namespace MI {

namespace MDL {

using mi::mdl::as;

Mdl_function_call::Mdl_function_call()
: m_tf(get_type_factory())
, m_vf(get_value_factory())
, m_ef(get_expression_factory())
, m_module_tag()
, m_definition_tag()
, m_function_index(~0u)
, m_mdl_semantic(mi::mdl::IDefinition::DS_UNKNOWN)
, m_definition_name()
, m_immutable(false) // avoid ubsan warning with swap() and temporaries
, m_parameter_types()
, m_return_type()
, m_arguments()
, m_enable_if_conditions()
{
}

template<typename T>
static inline T *check_non_null(T *obj)
{
    ASSERT(M_SCENE, obj && "NULL argument not allowed");
    return obj;
}

Mdl_function_call::Mdl_function_call(
    DB::Tag module_tag,
    DB::Tag definition_tag,
    mi::Uint32 function_index,
    IExpression_list* arguments,
    mi::mdl::IDefinition::Semantics semantic,
    const char* definition_name,
    const IType_list* parameter_types,
    const IType* return_type,
    bool immutable,
    const IExpression_list* enable_if_conditions)
: m_tf(get_type_factory())
, m_vf(get_value_factory())
, m_ef(get_expression_factory())
, m_module_tag(module_tag)
, m_definition_tag(definition_tag)
, m_function_index(function_index)
, m_mdl_semantic(semantic)
, m_definition_name(check_non_null(definition_name))
, m_immutable(immutable)
, m_parameter_types(make_handle_dup(parameter_types))
, m_return_type(make_handle_dup(return_type))
, m_arguments(make_handle_dup(arguments))
, m_enable_if_conditions(make_handle_dup(enable_if_conditions))
{
}

Mdl_function_call::Mdl_function_call( const Mdl_function_call& other)
: SCENE::Scene_element<Mdl_function_call, ID_MDL_FUNCTION_CALL>(other)
, m_tf(other.m_tf)
, m_vf(other.m_vf)
, m_ef(other.m_ef)
, m_module_tag(other.m_module_tag)
, m_definition_tag(other.m_definition_tag)
, m_function_index(other.m_function_index)
, m_mdl_semantic(other.m_mdl_semantic)
, m_definition_name(other.m_definition_name)
, m_immutable(other.m_immutable)
, m_parameter_types(other.m_parameter_types)
, m_return_type(other.m_return_type)
, m_arguments(m_ef->clone(
    other.m_arguments.get(), /*transaction*/ nullptr, /*copy_immutable_calls*/ false))
, m_enable_if_conditions(other.m_enable_if_conditions)  // shared, no clone necessary
{
}

DB::Tag Mdl_function_call::get_function_definition() const
{
    ASSERT( M_SCENE, m_definition_tag.is_valid());
    return m_definition_tag;
}

const char* Mdl_function_call::get_mdl_function_definition() const
{
    return m_definition_name.c_str();
}

const IType* Mdl_function_call::get_return_type() const
{
    m_return_type->retain();
    return m_return_type.get();
}

mi::Size Mdl_function_call::get_parameter_count() const
{
    return m_arguments->get_size();
}

const char* Mdl_function_call::get_parameter_name( mi::Size index) const
{
    return m_arguments->get_name( index);
}

mi::Size Mdl_function_call::get_parameter_index( const char* name) const
{
    return m_arguments->get_index( name);
}

const IType_list* Mdl_function_call::get_parameter_types() const
{
    m_parameter_types->retain();
    return m_parameter_types.get();
}

const IExpression_list* Mdl_function_call::get_arguments() const
{
    m_arguments->retain();
    return m_arguments.get();
}

// Get the list of enable_if conditions.
const IExpression_list* Mdl_function_call::get_enable_if_conditions() const
{
    m_enable_if_conditions->retain();
    return m_enable_if_conditions.get();
}

mi::Sint32 Mdl_function_call::set_arguments(
    DB::Transaction* transaction, const IExpression_list* arguments)
{
    if( !arguments)
        return -1;
    mi::Size n = arguments->get_size();
    for( mi::Size i = 0; i < n; ++i) {
        const char* name = arguments->get_name( i);
        mi::base::Handle<const IExpression> argument( arguments->get_expression( name));
        mi::Sint32 result = set_argument( transaction, name, argument.get());
        if( result != 0)
            return result;
    }
    return 0;
}

mi::Sint32 Mdl_function_call::set_argument(
    DB::Transaction* transaction, mi::Size index, const IExpression* argument)
{
   if( !argument)
        return -1;
    mi::base::Handle<const IType> expected_type( m_parameter_types->get_type( index));
    if( !expected_type)
        return -2;
    mi::base::Handle<const IType> actual_type( argument->get_type());
    if( !argument_type_matches_parameter_type(
        m_tf.get(), actual_type.get(), expected_type.get()))
        return -3;
    if( m_immutable)
        return -4;
    bool actual_type_varying   = (actual_type->get_all_type_modifiers()   & IType::MK_VARYING) != 0;
    bool expected_type_uniform = (expected_type->get_all_type_modifiers() & IType::MK_UNIFORM) != 0;
    if( actual_type_varying && expected_type_uniform)
        return -5;
    IExpression::Kind kind = argument->get_kind();
    if( kind != IExpression::EK_CONSTANT && kind != IExpression::EK_CALL)
        return -6;
    if( expected_type_uniform && return_type_is_varying( transaction, argument))
        return -8;
    mi::base::Handle<IExpression> argument_copy( m_ef->clone(
        argument, /*transaction*/ nullptr, /*copy_immutable_calls*/ false));
    m_arguments->set_expression( index, argument_copy.get());
    return 0;
}

mi::Sint32 Mdl_function_call::set_argument(
    DB::Transaction* transaction, const char* name, const IExpression* argument)
{
    if( !name || !argument)
        return -1;
    mi::Size index = get_parameter_index( name);
    return set_argument( transaction, index, argument);
}

void Mdl_function_call::make_mutable(DB::Transaction* transaction) {

    // function calls, which are defaults in their own module, do not
    // keep a reference to their module, get it now
    if (!m_module_tag.is_valid()) {
        DB::Access<Mdl_function_definition> definition(m_definition_tag, transaction);
        m_module_tag = definition->get_module(transaction);
        ASSERT(M_SCENE, m_module_tag.is_valid());
    }
    m_immutable = false;
}

mi::mdl::IDefinition::Semantics Mdl_function_call::get_mdl_semantic() const
{
    return m_mdl_semantic;
}

const mi::mdl::IType* Mdl_function_call::get_mdl_return_type( DB::Transaction* transaction) const
{
    DB::Access<Mdl_function_definition> definition( m_definition_tag, transaction);
    return definition.is_valid() ? definition->get_mdl_return_type( transaction) : 0;
}

const mi::mdl::IType* Mdl_function_call::get_mdl_parameter_type(
    DB::Transaction* transaction, mi::Uint32 index) const
{
    DB::Access<Mdl_function_definition> definition( m_definition_tag, transaction);
    return definition.is_valid() ? definition->get_mdl_parameter_type( transaction, index) : 0;
}

void Mdl_function_call::swap( Mdl_function_call& other)
{
    SCENE::Scene_element<Mdl_function_call, Mdl_function_call::id>::swap( other);

    std::swap( m_module_tag, other.m_module_tag);
    std::swap( m_definition_tag, other.m_definition_tag);
    std::swap( m_function_index, other.m_function_index);
    std::swap( m_mdl_semantic, other.m_mdl_semantic);
    m_definition_name.swap( other.m_definition_name);

    std::swap( m_immutable, other.m_immutable);
    std::swap( m_parameter_types, other.m_parameter_types);
    std::swap( m_return_type, other.m_return_type);
    std::swap( m_arguments, other.m_arguments);
    std::swap( m_enable_if_conditions, other.m_enable_if_conditions);
}

mi::mdl::IGenerated_code_lambda_function*
Mdl_function_call::create_jitted_function(
    DB::Transaction* transaction,
    bool environment_context,
    mi::Float32 mdl_meters_per_scene_unit,
    mi::Float32 mdl_wavelength_min,
    mi::Float32 mdl_wavelength_max,
    Sint32* errors) const
{
    mi::Sint32 dummy_errors;
    if( !errors)
        errors = &dummy_errors;
    *errors = -1;

    // get the JIT code generator
    SYSTEM::Access_module<MDLC::Mdlc_module> m_mdlc_module( false);
    mi::base::Handle<mi::mdl::IMDL> mdl( m_mdlc_module->get_mdl());
    mi::base::Handle<mi::mdl::ICode_generator_jit> generator_jit
        = mi::base::make_handle( mdl->load_code_generator( "jit"))
            .get_interface<mi::mdl::ICode_generator_jit>();
    if( !generator_jit.is_valid_interface()) {
        *errors = -1;
        return 0;
    }

    // get function definition and check its return type
    DB::Access<Mdl_function_definition> function_definition( m_definition_tag, transaction);
    const mi::mdl::IType* return_type = function_definition->get_mdl_return_type( transaction);
    bool is_ok = false;
    if( return_type->skip_type_alias()->get_kind() == mi::mdl::IType::TK_COLOR) {
        is_ok = true;
    } else if( const mi::mdl::IType_struct* s_type = as<mi::mdl::IType_struct>( return_type)) {
        // check for a type struct equal to ::base::texture_return
        if( s_type->get_field_count() == 2) {
            const mi::mdl::IType*   c_type;
            const mi::mdl::IType*   f_type;
            const mi::mdl::ISymbol* dummy;
            s_type->get_field( 0, c_type, dummy);
            s_type->get_field( 1, f_type, dummy);

            if( c_type->skip_type_alias()->get_kind() == mi::mdl::IType::TK_COLOR &&
                f_type->skip_type_alias()->get_kind() == mi::mdl::IType::TK_FLOAT) {
                is_ok = true;
            }
        }
        // enforce the name
        if( strcmp( s_type->get_symbol()->get_name(), "::base::texture_return") != 0) {
            is_ok = false;
        }
    }
    if( !is_ok) {
        *errors = -2;
        return 0;
    }

    // get code DAG
    ASSERT(M_SCENE, m_module_tag.is_valid());
    DB::Access<Mdl_module> module(m_module_tag, transaction);
    mi::base::Handle<const mi::mdl::IGenerated_code_dag> code_dag( module->get_code_dag());

    // create a lambda function for an environment function
    mi::base::Handle<mi::mdl::ILambda_function> lambda_func(
        mdl->create_lambda_function( environment_context
            ?  mi::mdl::ILambda_function::LEC_ENVIRONMENT
            : mi::mdl::ILambda_function::LEC_DISPLACEMENT ));

    // set JIT generator parameters
    SYSTEM::Access_module<CONFIG::Config_module> config_module( false);
    const CONFIG::Config_registry& registry = config_module->get_configuration();
    mi::mdl::Options& options = generator_jit->access_options();
    options.set_option( MDL_JIT_OPTION_ENABLE_RO_SEGMENT, "true");
    options.set_option( MDL_JIT_OPTION_USE_BITANGENT, "true");
    int jit_opt_level = 0;
    if( registry.get_value( "jit_opt_level", jit_opt_level)) {
        options.set_option( MDL_JIT_OPTION_OPT_LEVEL, std::to_string( jit_opt_level).c_str());
    }
    bool jit_fast_math = false;
    if( registry.get_value( "jit_fast_math", jit_fast_math)) {
        options.set_option( MDL_JIT_OPTION_FAST_MATH, jit_fast_math ? "true" : "false");
    }

    // convert m_arguments to DAG nodes
    mi::Uint32 n_params = code_dag->get_function_parameter_count( m_function_index);
    std::vector<mi::mdl::DAG_call::Call_argument> mdl_arguments( n_params);
    for( mi::Uint32 i = 0; i < n_params; ++i) {
        const char* parameter_name = code_dag->get_function_parameter_name( m_function_index, i);
        const mi::mdl::IType* parameter_type
            = code_dag->get_function_parameter_type( m_function_index, i);
        mi::base::Handle<const IExpression> argument( m_arguments->get_expression( parameter_name));
        const mi::mdl::DAG_node* arg = int_expr_to_mdl_dag_node(
            transaction, lambda_func.get(), parameter_type, argument.get(),
            mdl_meters_per_scene_unit, mdl_wavelength_min, mdl_wavelength_max);
        if( !arg) {
            LOG::mod_log->error( M_SCENE, LOG::Mod_log::C_DATABASE,
                "Type mismatch, call of an unsuitable DB element, or cycle in a graph rooted "
                "at the function definition \"%s\".",
                add_mdl_db_prefix( code_dag->get_function_name( m_function_index)).c_str());
            *errors = -3;
            return 0;
        }
        mdl_arguments[i].arg        = arg;
        mdl_arguments[i].param_name = parameter_name;
    }

    // create DAG node for the environment function
    const mi::mdl::DAG_node* call = lambda_func->create_call(
        code_dag->get_function_name( m_function_index), function_definition->get_mdl_semantic(),
        n_params > 0 ? &mdl_arguments[0] : NULL, n_params, return_type);

    // if return type is a struct type assume it is ::base::texture_return (see above) and wrap
    // DAG node by a select to extract the tint field
    if( const mi::mdl::IType_struct* s_type = as<mi::mdl::IType_struct>( return_type)) {
        const mi::mdl::IType*   f_type;
        const mi::mdl::ISymbol* f_name;

        s_type->get_field( 0, f_type, f_name);

        std::string name( s_type->get_symbol()->get_name());
        name += '.';
        name += f_name->get_name();
        name += '(';
        name += s_type->get_symbol()->get_name();
        name += ')';

        mi::mdl::DAG_call::Call_argument args[1];

        args[0].arg        = call;
        args[0].param_name = "s";
        call = lambda_func->create_call(
            name.c_str(), mi::mdl::IDefinition::DS_INTRINSIC_DAG_FIELD_ACCESS, args, 1, f_type);
    }

    if( !environment_context )
    {
#ifdef ENABLE_ASSERT
        size_t idx =
#endif
            lambda_func->store_root_expr( call);
        ASSERT( M_SCENE, idx == 0 );
    } else {
        lambda_func->set_body( call);
    }

    // compile the environment lambda function
    Mdl_call_resolver resolver( transaction);
    mi::mdl::IGenerated_code_lambda_function* jitted_func;
    if( !environment_context )
        jitted_func = generator_jit->compile_into_switch_function(
            lambda_func.get(), &resolver, 1, 0);
    else
        jitted_func = generator_jit->compile_into_environment( lambda_func.get(), &resolver);
    if( !jitted_func) {
        *errors = -4;
        return 0;
    }

    *errors = 0;
    return jitted_func;
}

const SERIAL::Serializable* Mdl_function_call::serialize( SERIAL::Serializer* serializer) const
{
    Scene_element_base::serialize( serializer);

    serializer->write( m_module_tag);
    serializer->write( m_definition_tag);
    serializer->write( m_function_index);
    serializer->write( static_cast<mi::Uint32>( m_mdl_semantic));
    serializer->write( m_definition_name);
    serializer->write( m_immutable);

    m_tf->serialize_list( serializer, m_parameter_types.get());
    m_tf->serialize( serializer, m_return_type.get());
    m_ef->serialize_list( serializer, m_arguments.get());
    m_ef->serialize_list( serializer, m_enable_if_conditions.get());

    return this + 1;
}

SERIAL::Serializable* Mdl_function_call::deserialize( SERIAL::Deserializer* deserializer)
{
    Scene_element_base::deserialize( deserializer);

    deserializer->read( &m_module_tag);
    deserializer->read( &m_definition_tag);
    deserializer->read( &m_function_index);
    mi::Uint32 semantic;
    deserializer->read( &semantic);
    m_mdl_semantic = static_cast<mi::mdl::IDefinition::Semantics>( semantic);
    deserializer->read( &m_definition_name);
    deserializer->read( &m_immutable);

    m_parameter_types = m_tf->deserialize_list( deserializer);
    m_return_type = m_tf->deserialize( deserializer);
    m_arguments = m_ef->deserialize_list( deserializer);
    m_enable_if_conditions = m_ef->deserialize_list( deserializer);

    return this + 1;
}

void Mdl_function_call::dump( DB::Transaction* transaction) const
{
    std::ostringstream s;
    s << std::boolalpha;
    mi::base::Handle<const mi::IString> tmp;

    s << "MDL module tag: " << m_module_tag.get_uint() << std::endl;
    s << "Function definition tag: " << m_definition_tag.get_uint() << std::endl;
    s << "Function definition MDL name: \"" << m_definition_name << "\"" << std::endl;
    tmp = m_ef->dump( transaction, m_arguments.get(), /*name*/ 0);
    s << "Arguments: " << tmp->get_c_str() << std::endl;
    s << "Immutable: " << m_immutable << std::endl;
    tmp = m_ef->dump( transaction, m_enable_if_conditions.get(), /*name*/ 0);
    s << "Enable_if conditions: " << tmp->get_c_str() << std::endl;

    LOG::mod_log->info( M_SCENE, LOG::Mod_log::C_DATABASE, "%s", s.str().c_str());
}

size_t Mdl_function_call::get_size() const
{
    return sizeof( *this)
        + SCENE::Scene_element<Mdl_function_call, Mdl_function_call::id>::get_size()
            - sizeof( SCENE::Scene_element<Mdl_function_call, Mdl_function_call::id>)
        + dynamic_memory_consumption( m_definition_name)
        + dynamic_memory_consumption( m_parameter_types)
        + dynamic_memory_consumption( m_return_type)
        + dynamic_memory_consumption( m_arguments)
        + dynamic_memory_consumption( m_enable_if_conditions);
}

DB::Journal_type Mdl_function_call::get_journal_flags() const
{
    return SCENE::JOURNAL_CHANGE_SHADER_ATTRIBUTE;
}

Uint Mdl_function_call::bundle( DB::Tag* results, Uint size) const
{
    return 0;
}

void Mdl_function_call::get_scene_element_references( DB::Tag_set* result) const
{
  
    // default functions are held by the module, avoid cycle.
    if (!m_immutable) { 
        ASSERT( M_SCENE, m_module_tag);
        result->insert(m_module_tag);
    }
    collect_references( m_arguments.get(), result);
    collect_references( m_enable_if_conditions.get(), result);
}

} // namespace MDL

} // namespace MI
