#pragma once
// Compatibility shims for the modern MSVC STL.
//
// The codebase targets the v142-era STL, which still shipped the C++03
// functional helpers under /std:c++14. Current toolsets removed them outright
// (the _HAS_AUTO_PTR_ETC escape hatch is gone too), so the legacy multiplayer
// and luabind code stopped compiling. This header restores the exact C++03
// shapes and is force-included (/FI) into every C++ TU by the build system.
//
// If a future STL brings these back, the include guard below must be extended
// with a version check instead of reopening namespace std unconditionally.

#if defined(_MSC_VER)

#include <memory>
#include <functional>

#if !defined(_HAS_AUTO_PTR_ETC) || (_HAS_AUTO_PTR_ETC == 0)

namespace std
{
	template <class Arg, class Result>
	struct unary_function
	{
		typedef Arg    argument_type;
		typedef Result result_type;
	};

	template <class Arg1, class Arg2, class Result>
	struct binary_function
	{
		typedef Arg1   first_argument_type;
		typedef Arg2   second_argument_type;
		typedef Result result_type;
	};

	// --- binders ---------------------------------------------------------------
	template <class Fn>
	class binder1st : public unary_function<typename Fn::second_argument_type, typename Fn::result_type>
	{
	protected:
		Fn op;
		typename Fn::first_argument_type value;
	public:
		binder1st(const Fn& f, const typename Fn::first_argument_type& v) : op(f), value(v) {}
		typename Fn::result_type operator()(const typename Fn::second_argument_type& x) const { return op(value, x); }
		typename Fn::result_type operator()(typename Fn::second_argument_type& x) const { return op(value, x); }
	};

	template <class Fn, class T>
	binder1st<Fn> bind1st(const Fn& f, const T& v)
	{
		return binder1st<Fn>(f, typename Fn::first_argument_type(v));
	}

	template <class Fn>
	class binder2nd : public unary_function<typename Fn::first_argument_type, typename Fn::result_type>
	{
	protected:
		Fn op;
		typename Fn::second_argument_type value;
	public:
		binder2nd(const Fn& f, const typename Fn::second_argument_type& v) : op(f), value(v) {}
		typename Fn::result_type operator()(const typename Fn::first_argument_type& x) const { return op(x, value); }
		typename Fn::result_type operator()(typename Fn::first_argument_type& x) const { return op(x, value); }
	};

	template <class Fn, class T>
	binder2nd<Fn> bind2nd(const Fn& f, const T& v)
	{
		return binder2nd<Fn>(f, typename Fn::second_argument_type(v));
	}

	// --- mem_fun / ptr_fun -----------------------------------------------------
	template <class Result, class T>
	class mem_fun_t : public unary_function<T*, Result>
	{
		Result (T::*pm)();
	public:
		explicit mem_fun_t(Result (T::*p)()) : pm(p) {}
		Result operator()(T* p) const { return (p->*pm)(); }
	};

	template <class Result, class T>
	class const_mem_fun_t : public unary_function<const T*, Result>
	{
		Result (T::*pm)() const;
	public:
		explicit const_mem_fun_t(Result (T::*p)() const) : pm(p) {}
		Result operator()(const T* p) const { return (p->*pm)(); }
	};

	template <class Result, class T, class Arg>
	class mem_fun1_t : public binary_function<T*, Arg, Result>
	{
		Result (T::*pm)(Arg);
	public:
		explicit mem_fun1_t(Result (T::*p)(Arg)) : pm(p) {}
		Result operator()(T* p, Arg a) const { return (p->*pm)(a); }
	};

	template <class Result, class T, class Arg>
	class const_mem_fun1_t : public binary_function<const T*, Arg, Result>
	{
		Result (T::*pm)(Arg) const;
	public:
		explicit const_mem_fun1_t(Result (T::*p)(Arg) const) : pm(p) {}
		Result operator()(const T* p, Arg a) const { return (p->*pm)(a); }
	};

	template <class Result, class T>
	mem_fun_t<Result, T> mem_fun(Result (T::*p)()) { return mem_fun_t<Result, T>(p); }
	template <class Result, class T>
	const_mem_fun_t<Result, T> mem_fun(Result (T::*p)() const) { return const_mem_fun_t<Result, T>(p); }
	template <class Result, class T, class Arg>
	mem_fun1_t<Result, T, Arg> mem_fun(Result (T::*p)(Arg)) { return mem_fun1_t<Result, T, Arg>(p); }
	template <class Result, class T, class Arg>
	const_mem_fun1_t<Result, T, Arg> mem_fun(Result (T::*p)(Arg) const) { return const_mem_fun1_t<Result, T, Arg>(p); }

	template <class Arg, class Result>
	class pointer_to_unary_function : public unary_function<Arg, Result>
	{
		Result (*fn)(Arg);
	public:
		explicit pointer_to_unary_function(Result (*f)(Arg)) : fn(f) {}
		Result operator()(Arg a) const { return fn(a); }
	};

	template <class Arg1, class Arg2, class Result>
	class pointer_to_binary_function : public binary_function<Arg1, Arg2, Result>
	{
		Result (*fn)(Arg1, Arg2);
	public:
		explicit pointer_to_binary_function(Result (*f)(Arg1, Arg2)) : fn(f) {}
		Result operator()(Arg1 a, Arg2 b) const { return fn(a, b); }
	};

	template <class Arg, class Result>
	pointer_to_unary_function<Arg, Result> ptr_fun(Result (*f)(Arg)) { return pointer_to_unary_function<Arg, Result>(f); }
	template <class Arg1, class Arg2, class Result>
	pointer_to_binary_function<Arg1, Arg2, Result> ptr_fun(Result (*f)(Arg1, Arg2)) { return pointer_to_binary_function<Arg1, Arg2, Result>(f); }

	// --- auto_ptr (classic ownership-transferring copy semantics) --------------
	template <class T>
	class auto_ptr
	{
		T* ptr;
	public:
		typedef T element_type;
		explicit auto_ptr(T* p = 0) throw() : ptr(p) {}
		auto_ptr(auto_ptr& other) throw() : ptr(other.release()) {}
		template <class U>
		auto_ptr(auto_ptr<U>& other) throw() : ptr(other.release()) {}
		~auto_ptr() { delete ptr; }

		auto_ptr& operator=(auto_ptr& other) throw() { reset(other.release()); return *this; }
		template <class U>
		auto_ptr& operator=(auto_ptr<U>& other) throw() { reset(other.release()); return *this; }

		T& operator*() const throw() { return *ptr; }
		T* operator->() const throw() { return ptr; }
		T* get() const throw() { return ptr; }
		T* release() throw() { T* t = ptr; ptr = 0; return t; }
		void reset(T* p = 0) throw() { if (p != ptr) { delete ptr; ptr = p; } }
	};
}

#endif // !_HAS_AUTO_PTR_ETC
#endif // _MSC_VER
