﻿// © 2020-2021 Uniontech Software Technology Co.,Ltd.

#include "Evaluation.h" // for TermTags, ReductionStatus, TermNode, Context,
//	TermToNamePtr, ystdex::sfmt, std::string, Unilang::TryAccessLeaf
//	TermReference, ContextHandler, yunseq, std::prev, GetLValueTagsOf,
//	EnvironmentReference, in_place_type, ThrowTypeErrorForInvalidType,
//	ThrowInsufficientTermsError, Unilang::TryAccessTerm, ystdex::begins_with;
#include <cassert> // for assert;
#include <ystdex/cctype.h> // for ystdex::isdigit;
#include "Exception.h" // for BadIdentifier, InvalidReference,
//	InvalidSyntax, std::throw_with_nested, ParameterMismatch,
//	ListReductionFailure;
#include "Lexical.h" // for IsUnilangSymbol;
#include <ystdex/type_traits.hpp> // for ystdex::false_, ystdex::true_;
#include <ystdex/typeinfo.h> // for ystdex::type_id;
#include <ystdex/functional.hpp> // for ystdex::update_thunk;
#include "TCO.h" // for RelayDirect;

namespace Unilang
{

namespace
{

[[nodiscard, gnu::pure]] inline TermReference
EnsureLValueReference(TermReference&& ref)
{
	return TermReference(ref.GetTags() & ~TermTags::Unique, std::move(ref));
}

ReductionStatus
ReduceLeaf(TermNode& term, Context& ctx)
{
	if(const auto p = TermToNamePtr(term))
	{
		auto res(ReductionStatus::Retained);
		const auto id(*p);

		assert(id.data());

		if(!id.empty())
		{
			const char f(id.front());

			if((id.size() > 1 && (f == '#'|| f == '+' || f == '-')
				&& id.find_first_not_of("+-") != string_view::npos)
				|| ystdex::isdigit(f))
				throw InvalidSyntax(ystdex::sfmt<std::string>(id.front()
					!= '#' ? "Unsupported literal prefix found in literal '%s'."
					: "Invalid literal '%s' found.", id.data()));

			auto pr(ctx.Resolve(ctx.GetRecordPtr(), id));

			if(pr.first)
			{
				auto& bound(*pr.first);

				if(const auto p_bound
					= Unilang::TryAccessLeaf<const TermReference>(bound))
				{
					term.GetContainerRef() = bound.GetContainer();
					term.Value = EnsureLValueReference(TermReference(*p_bound));
				}
				else
				{
					auto p_env(Unilang::Nonnull(pr.second));

					term.Value = TermReference(p_env->MakeTermTags(bound)
						& ~TermTags::Unique, bound, std::move(p_env));
				}
				res = ReductionStatus::Neutral;
			}
			else
				throw BadIdentifier(id);
		}
		return CheckReducible(res) ? ReduceOnce(term, ctx) : res;
	}
	return ReductionStatus::Retained;
}

template<typename... _tParams>
ReductionStatus
CombinerReturnThunk(const ContextHandler& h, TermNode& term, Context& ctx,
	_tParams&&... args)
{
	static_assert(sizeof...(args) < 2, "Unsupported owner arguments found.");

	auto& act(EnsureTCOAction(ctx, term));
	auto& lf(act.LastFunction);

	term.Value.Clear();
	lf = {};
	yunseq(0,
		(lf = &act.AttachFunction(std::forward<_tParams>(args)).get(), 0)...);
	ctx.SetNextTermRef(term);
	return RelaySwitched(ctx, Continuation(std::ref(lf ? *lf : h), ctx));
}

ReductionStatus
ReduceBranch(TermNode& term, Context& ctx)
{
	if(IsBranch(term))
	{
		assert(term.size() != 0);
		if(term.size() == 1)
		{
			// NOTE: The following is necessary to prevent unbounded overflow in
			//	handling recursive subterms.
			auto term_ref(ystdex::ref(term));

			do
			{
				term_ref = AccessFirstSubterm(term_ref);
			}
			while(term_ref.get().size() == 1);
			return ReduceOnceLifted(term, ctx, term_ref);
		}
		if(IsEmpty(AccessFirstSubterm(term)))
			RemoveHead(term);
		assert(IsBranchedList(term));
		ctx.SetNextTermRef(term);
		ctx.LastStatus = ReductionStatus::Neutral;

		auto& sub(AccessFirstSubterm(term));

		ctx.SetupFront([&](Context& c){
			c.SetNextTermRef(term);
			return ReduceCombinedBranch(term, ctx);
		});
		ctx.SetupFront([&]{
			return ReduceOnce(sub, ctx);
		});
		return ReductionStatus::Partial;
	}
	return ReductionStatus::Retained;
}


inline ReductionStatus
ReduceChildrenOrderedAsync(TNIter, TNIter, Context&);

ReductionStatus
ReduceChildrenOrderedAsyncUnchecked(TNIter first, TNIter last, Context& ctx)
{
	assert(first != last);

	auto& term(*first++);

	return ReduceSubsequent(term, ctx, [=](Context& c){
		return ReduceChildrenOrderedAsync(first, last, c);
	});
}

inline ReductionStatus
ReduceChildrenOrderedAsync(TNIter first, TNIter last, Context& ctx)
{
	return first != last ? ReduceChildrenOrderedAsyncUnchecked(first, last, ctx)
		: ReductionStatus::Neutral;
}

ReductionStatus
ReduceSequenceOrderedAsync(TermNode& term, Context& ctx, TNIter i)
{
	assert(i != term.end());
	if(std::next(i) == term.end())
		return ReduceOnceLifted(term, ctx, *i);
	ctx.SetupFront([&, i]{
		return ReduceSequenceOrderedAsync(term, ctx, term.erase(i));
	});
	return ReduceOnce(*i, ctx);
}


inline void
CopyTermTags(TermNode& term, const TermNode& tm) noexcept
{
	term.Tags = GetLValueTagsOf(tm.Tags);
}

[[nodiscard, gnu::const]] constexpr TermTags
BindReferenceTags(TermTags ref_tags) noexcept
{
	return bool(ref_tags & TermTags::Unique) ? ref_tags | TermTags::Temporary
		: ref_tags;
}
[[nodiscard, gnu::pure]] inline TermTags
BindReferenceTags(const TermReference& ref) noexcept
{
	return BindReferenceTags(GetLValueTagsOf(ref.GetTags()));
}


using Action = function<void()>;

void
MarkTemporaryTerm(TermNode& term, char sigil) noexcept
{
	if(sigil != char())
		term.Tags |= TermTags::Temporary;
}

struct BindParameterObject
{
	lref<const EnvironmentReference> Referenced;

	BindParameterObject(const EnvironmentReference& r_env)
		: Referenced(r_env)
	{}

	template<typename _fCopy, typename _fMove>
	void
	operator()(char sigil, bool ref_temp, TermTags o_tags, TermNode& o,
		_fCopy cp, _fMove mv) const
	{
		const bool temp(bool(o_tags & TermTags::Temporary));

		if(sigil != '@')
		{
			const bool can_modify(!bool(o_tags & TermTags::Nonmodifying));
			const auto a(o.get_allocator());

			if(const auto p = Unilang::TryAccessLeaf<TermReference>(o))
			{
				if(sigil != char())
				{
					const auto ref_tags(PropagateTo(ref_temp
						? BindReferenceTags(*p) : p->GetTags(), o_tags));

					if(can_modify && temp)
						mv(std::move(o.GetContainerRef()),
							ValueObject(std::allocator_arg, a, in_place_type<
							TermReference>, ref_tags, std::move(*p)));
					else
						mv(TermNode::Container(o.GetContainer()),
							ValueObject(std::allocator_arg, a,
							in_place_type<TermReference>, ref_tags, *p));
				}
				else
				{
					auto& src(p->get());

					if(!p->IsMovable())
						cp(src);
					else
						mv(std::move(src.GetContainerRef()),
							std::move(src.Value));
				}
			}
			else if((can_modify || sigil == '%') && temp)
				MarkTemporaryTerm(mv(std::move(o.GetContainerRef()),
					std::move(o.Value)), sigil);
			else if(sigil == '&')
				mv(TermNode::Container(o.get_allocator()),
					ValueObject(std::allocator_arg, o.get_allocator(),
					in_place_type<TermReference>,
					GetLValueTagsOf(o.Tags | o_tags), o, Referenced));
			else
				cp(o);
		}
		else if(!temp)
			mv(TermNode::Container(o.get_allocator()),
				ValueObject(std::allocator_arg, o.get_allocator(),
				in_place_type<TermReference>, o_tags & TermTags::Nonmodifying,
				o, Referenced));
		else
			throw
				InvalidReference("Invalid operand found on binding sigil '@'.");
	}
};


[[noreturn]] inline void
ThrowFormalParameterTypeError(const TermNode& term, bool has_ref)
{
	ThrowTypeErrorForInvalidType(ystdex::type_id<TokenValue>(), term, has_ref);
}

[[noreturn]] void
ThrowNestedParameterTreeCheckError()
{
	std::throw_with_nested(InvalidSyntax("Failed checking for parameter in a"
		" parameter tree (expected a symbol or '#ignore')."));
}

struct ParameterCheck final
{
	using HasReferenceArg = ystdex::true_;

	static void
	CheckBack(const TermNode& t, bool t_has_ref)
	{
		if(!IsList(t))
			ThrowFormalParameterTypeError(t, t_has_ref);
	}

	template<typename _func>
	static void
	HandleLeaf(_func f, const TermNode& t, bool t_has_ref)
	{
		if(const auto p = TermToNamePtr(t))
		{
			const auto& n(*p);

			if(!IsIgnore(n))
			{
				if(IsUnilangSymbol(n))
					f(n);
				else
					ThrowInvalidTokenError(n);
			}
		}
		else
			ThrowFormalParameterTypeError(t, t_has_ref);
	}

	template<typename _func>
	static void
	WrapCall(_func f)
	{
		try
		{
			f();
		}
		catch(ParameterMismatch&)
		{
			throw;
		}
		catch(...)
		{
			ThrowNestedParameterTreeCheckError();
		}
	}
};


struct NoParameterCheck final
{
	using HasReferenceArg = ystdex::false_;

	static void
	CheckBack(const TermNode& t)
	{
		yunused(t);
		assert(IsList(t));
	}

	template<typename _func>
	static void
	HandleLeaf(_func f, const TermNode& t)
	{
		const auto p(TermToNamePtr(t));

		assert(bool(p));

		const auto& n(*p);

		if(!IsIgnore(n))
		{
			assert(IsUnilangSymbol(n));
			f(n);
		}
	}

	template<typename _func>
	static void
	WrapCall(_func f)
	{
		f();
	}
};


template<class _tTraits, typename _fBindTrailing, typename _fBindValue>
class GParameterMatcher
{
public:
	_fBindTrailing BindTrailing;
	_fBindValue BindValue;

private:
	mutable Action act{};

public:
	template<class _type, class _type2>
	GParameterMatcher(_type&& arg, _type2&& arg2)
		: BindTrailing(yforward(arg)), BindValue(yforward(arg2))
	{}

	void
	operator()(const TermNode& t, TermNode& o, TermTags o_tags,
		const EnvironmentReference& r_env) const
	{
		_tTraits::WrapCall([&]{
			DispatchMatch(typename _tTraits::HasReferenceArg(), t, o, o_tags,
				r_env, ystdex::false_());
			while(act)
			{
				const auto a(std::move(act));

				a();
			}
		});
	}

private:
	template<class _tArg>
	void
	DispatchMatch(ystdex::true_, const TermNode& t, TermNode& o,
		TermTags o_tags, const EnvironmentReference& r_env, _tArg) const
	{
		Match(t, o, o_tags, r_env, _tArg::value);
	}
	template<class _tArg>
	void
	DispatchMatch(ystdex::false_, const TermNode& t, TermNode& o,
		TermTags o_tags, const EnvironmentReference& r_env, _tArg) const
	{
		Match(t, o, o_tags, r_env);
	}

	template<typename... _tParams>
	void
	Match(const TermNode& t, TermNode& o, TermTags o_tags,
		const EnvironmentReference& r_env, _tParams&&... args) const
	{
		if(IsList(t))
		{
			if(IsBranch(t))
			{
				const auto n_p(t.size());
				auto last(t.end());

				if(n_p > 0)
				{
					const auto& back(*std::prev(last));

					if(IsLeaf(back))
					{
						if(const auto p
							= Unilang::TryAccessLeaf<TokenValue>(back))
						{
							if(!p->empty() && p->front() == '.')
								--last;
						}
						else
							_tTraits::CheckBack(back, yforward(args)...);
					}
				}
				ResolveTerm([&, n_p, o_tags](TermNode& nd,
					ResolvedTermReferencePtr p_ref){
					if(IsList(nd))
					{
						const bool ellipsis(last != t.end());
						const auto n_o(nd.size());

						if(n_p == n_o || (ellipsis && n_o >= n_p - 1))
						{
							auto tags(o_tags);

							if(p_ref)
							{
								const auto ref_tags(p_ref->GetTags());

								tags = (tags
									& ~(TermTags::Unique | TermTags::Temporary))
									| (ref_tags & TermTags::Unique);
								tags = PropagateTo(tags, ref_tags);
							}
							MatchSubterms(t.begin(), last, nd, nd.begin(), tags,
								p_ref ? p_ref->GetEnvironmentReference()
								: r_env, ellipsis);
						}
						else if(!ellipsis)
							throw ArityMismatch(n_p, n_o);
						else
							ThrowInsufficientTermsError(nd, p_ref);
					}
					else
						ThrowListTypeErrorForNonlist(nd, p_ref);
				}, o);
			}
			else
				ResolveTerm([&](const TermNode& nd, bool has_ref){
					if(nd)
						throw ParameterMismatch(ystdex::sfmt("Invalid nonempty"
							" operand value '%s' found for empty list"
							" parameter.", TermToStringWithReferenceMark(nd,
							has_ref).c_str()));
				}, o);
		}
		else if(const auto p_t = Unilang::TryAccessLeaf<const TermReference>(t))
		{
			auto& nd(p_t->get());

			ystdex::update_thunk(act, [&, o_tags]{
				DispatchMatch(typename _tTraits::HasReferenceArg(), nd, o,
					o_tags, r_env, ystdex::true_());
			});
		}
		else
			_tTraits::HandleLeaf([&](const TokenValue& n){
				BindValue(n, o, o_tags, r_env);
			}, t, yforward(args)...);
	}

	void
	MatchSubterms(TNCIter i, TNCIter last, TermNode& o_tm, TNIter j,
		TermTags tags, const EnvironmentReference& r_env, bool ellipsis) const
	{
		if(i != last)
		{
			ystdex::update_thunk(act,
				[this, i, j, last, tags, ellipsis, &o_tm, &r_env]{
				return MatchSubterms(std::next(i), last, o_tm, std::next(j),
					tags, r_env, ellipsis);
			});
			assert(j != o_tm.end());
			DispatchMatch(typename _tTraits::HasReferenceArg(), Unilang::Deref(i),
				Unilang::Deref(j), tags, r_env, ystdex::false_());
		}
		else if(ellipsis)
		{
			const auto& lastv(last->Value);

			assert(lastv.type() == ystdex::type_id<TokenValue>());
			BindTrailing(o_tm, j, lastv.GetObject<TokenValue>(), tags, r_env);
		}
	}
};

template<class _tTraits, typename _fBindTrailing, typename _fBindValue>
[[nodiscard]] inline GParameterMatcher<_tTraits, _fBindTrailing, _fBindValue>
MakeParameterMatcher(_fBindTrailing bind_trailing_seq, _fBindValue bind_value)
{
	return GParameterMatcher<_tTraits, _fBindTrailing, _fBindValue>(
		std::move(bind_trailing_seq), std::move(bind_value));
}

char
ExtractSigil(string_view& id)
{
	char sigil(id.front());

	if(sigil != '&' && sigil != '%' && sigil != '@')
		sigil = char();
	else
		id.remove_prefix(1);
	return sigil;
}

template<class _tTraits>
void
BindParameterImpl(const shared_ptr<Environment>& p_env, const TermNode& t,
	TermNode& o)
{
	auto& env(Unilang::Deref(p_env));

	MakeParameterMatcher<_tTraits>([&](TermNode& o_tm, TNIter first,
		string_view id, TermTags o_tags, const EnvironmentReference& r_env){
		assert(ystdex::begins_with(id, "."));
		id.remove_prefix(1);
		if(!id.empty())
		{
			const char sigil(ExtractSigil(id));

			if(!id.empty())
			{
				const auto a(o_tm.get_allocator());
				const auto last(o_tm.end());
				TermNode::Container con(a);

				if((o_tags & (TermTags::Unique | TermTags::Nonmodifying))
					== TermTags::Unique || bool(o_tags & TermTags::Temporary))
				{
					if(sigil == char())
						LiftSubtermsToReturn(o_tm);
					con.splice(con.end(), o_tm.GetContainerRef(), first, last);
					MarkTemporaryTerm(env.Bind(id, TermNode(std::move(con))),
						sigil);
				}
				else
				{
					for(; first != last; ++first)
						BindParameterObject{r_env}(sigil, {}, o_tags,
							Unilang::Deref(first), [&](const TermNode& tm){
							CopyTermTags(con.emplace_back(tm.GetContainer(),
								tm.Value), tm);
						}, [&](TermNode::Container&& c, ValueObject&& vo)
							-> TermNode&{
							con.emplace_back(std::move(c), std::move(vo));
							return con.back();
						});
					if(sigil == '&')
					{
						auto p_sub(YSLib::allocate_shared<TermNode>(a,
							std::move(con)));
						auto& sub(Unilang::Deref(p_sub));

						env.Bind(id, TermNode(std::allocator_arg, a,
							{Unilang::AsTermNode(a, std::move(p_sub))},
							std::allocator_arg, a, TermReference(sub, r_env)));
					}
					else
						MarkTemporaryTerm(env.Bind(id,
							TermNode(std::move(con))), sigil);
				}
			}
		}
	}, [&](const TokenValue& n, TermNode& b, TermTags o_tags,
		const EnvironmentReference& r_env){
		assert(!IsIgnore(n) && IsUnilangSymbol(n));

		string_view id(n);
		const char sigil(ExtractSigil(id));

		if(!id.empty())
			BindParameterObject{r_env}(sigil, sigil == '&', o_tags, b,
				[&](const TermNode& tm){
				CopyTermTags(env.Bind(id, tm), tm);
			}, [&](TermNode::Container&& c, ValueObject&& vo) -> TermNode&{
				return env.Bind(id, TermNode(std::move(c), std::move(vo)));
			});
	})(t, o, TermTags::Temporary, p_env);
}

} // unnamed namespace;

ReductionStatus
ReduceCombinedBranch(TermNode& term, Context& ctx)
{
	assert(IsBranchedList(term));

	auto& fm(AccessFirstSubterm(term));
	const auto p_ref_fm(Unilang::TryAccessLeaf<const TermReference>(fm));

	if(p_ref_fm)
	{
		term.Tags &= ~TermTags::Temporary;
		if(const auto p_handler
			= Unilang::TryAccessLeaf<const ContextHandler>(p_ref_fm->get()))
			return CombinerReturnThunk(*p_handler, term, ctx);
	}
	else
		term.Tags |= TermTags::Temporary;
	if(const auto p_handler = Unilang::TryAccessTerm<ContextHandler>(fm))
		return
			CombinerReturnThunk(*p_handler, term, ctx, std::move(*p_handler));
	assert(IsBranch(term));
	return ResolveTerm([&](const TermNode& nd, bool has_ref)
		YB_ATTR_LAMBDA(noreturn) -> ReductionStatus{
		throw ListReductionFailure(ystdex::sfmt("No matching combiner '%s'"
			" for operand with %zu argument(s) found.",
			TermToStringWithReferenceMark(nd, has_ref).c_str(),
			term.size() - 1));
	}, fm);
}

ReductionStatus
ReduceOnce(TermNode& term, Context& ctx)
{
	ctx.SetNextTermRef(term);
	return RelayDirect(ctx, ctx.ReduceOnce, term);
}

// NOTE: See Context.h for the declaration.
ReductionStatus
Context::DefaultReduceOnce(TermNode& term, Context& ctx)
{
	return term.Value ? ReduceLeaf(term, ctx) : ReduceBranch(term, ctx);
}

ReductionStatus
ReduceOrdered(TermNode& term, Context& ctx)
{
	if(IsBranch(term))
		return ReduceSequenceOrderedAsync(term, ctx, term.begin());
	term.Value = ValueToken::Unspecified;
	return ReductionStatus::Retained;
}


ReductionStatus
FormContextHandler::CallN(size_t n, TermNode& term, Context& ctx) const
{
	if(n == 0 || term.size() <= 1)
		return Handler(ctx.GetNextTermRef(), ctx);
	ctx.SetupFront([&, n](Context& c){
		c.SetNextTermRef( term);
		return CallN(n - 1, term, c);
	});
	ctx.SetNextTermRef(term);
	assert(!term.empty());
	ReduceChildrenOrderedAsyncUnchecked(std::next(term.begin()), term.end(),
		ctx);
	return ReductionStatus::Partial;
}


void
BindParameter(const shared_ptr<Environment>& p_env, const TermNode& t,
	TermNode& o)
{
	BindParameterImpl<ParameterCheck>(p_env, t, o);
}

void
BindParameterWellFormed(const shared_ptr<Environment>& p_env, const TermNode& t,
	TermNode& o)
{
	BindParameterImpl<NoParameterCheck>(p_env, t, o);
}

} // namespace Unilang;

