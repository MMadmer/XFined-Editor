// Copyright (c) 2003 Daniel Wallin and Arvid Norberg

// Permission is hereby granted, free of charge, to any person obtaining a
// copy of this software and associated documentation files (the "Software"),
// to deal in the Software without restriction, including without limitation
// the rights to use, copy, modify, merge, publish, distribute, sublicense,
// and/or sell copies of the Software, and to permit persons to whom the
// Software is furnished to do so, subject to the following conditions:

// The above copyright notice and this permission notice shall be included
// in all copies or substantial portions of the Software.

// THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF
// ANY KIND, EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED
// TO THE WARRANTIES OF MERCHANTABILITY, FITNESS FOR A
// PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT
// SHALL THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR
// ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN
// ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
// OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE
// OR OTHER DEALINGS IN THE SOFTWARE.

#include "pch.h"

#include <cstddef>
#include <type_traits>
#include <utility>

#include "luabind/detail/class_rep.hpp"
#include "luabind/detail/object_rep.hpp"
#include "luabind/detail/link_compatibility.hpp"
#include "luabind/error.hpp"
#include "luabind/object.hpp"

namespace
{
    using allocator_function = void* (__cdecl*)(void*, const void*, std::size_t);

    // These checks pin the X-Ray-specific API and allocation contracts used by editor and game modules.
    static_assert(std::is_same_v<luabind::memory_allocation_function_parameter, void*>);
    static_assert(std::is_same_v<luabind::memory_allocation_function_pointer, allocator_function>);
    static_assert(std::is_same_v<decltype(luabind::allocator), allocator_function>);
    static_assert(std::is_same_v<decltype(luabind::allocator_parameter), void*>);
    static_assert(std::is_same_v<typename luabind::internal_string::allocator_type, luabind::memory_allocator<char>>);
    static_assert(std::is_same_v<typename luabind::internal_vector<int>::allocator_type, luabind::memory_allocator<int>>);
    static_assert(std::is_same_v<luabind::pcall_callback_fun, int (*)(lua_State*)>);
    static_assert(std::is_same_v<luabind::pregister_callback_fun, void (*)(lua_State*, bool)>);
    static_assert(std::is_same_v<decltype(std::declval<const luabind::functor<void>&>().lua_state()), lua_State*>);

#ifndef LUABIND_NO_EXCEPTIONS
    static_assert(std::is_same_v<decltype(std::declval<const luabind::error&>().state()), lua_State*>);
#endif

#if defined(_MSC_VER) && defined(_M_X64)
    static_assert(sizeof(luabind::detail::class_rep) == 584);
    static_assert(sizeof(luabind::detail::object_rep) == 72);
    static_assert(sizeof(luabind::object) == 24);
    static_assert(sizeof(luabind::functor<void>) == 24);
    static_assert(luabind::detail::object_rep::constant == 1);
    static_assert(luabind::detail::object_rep::owner == 2);
    static_assert(luabind::detail::object_rep::lua_class == 4);
    static_assert(luabind::detail::object_rep::call_super == 8);
#endif
}

namespace luabind { namespace detail
{

#ifdef LUABIND_NOT_THREADSAFE
	void not_threadsafe_defined_conflict() {}
#else
	void not_threadsafe_not_defined_conflict() {}
#endif

#ifdef LUABIND_NO_ERROR_CHECKING
	void no_error_checking_defined_conflict() {}
#else
	void no_error_checking_not_defined_conflict() {}
#endif

}}

