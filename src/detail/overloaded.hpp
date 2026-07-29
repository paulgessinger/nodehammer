#pragma once

namespace nodehammer::detail {

/// Visitor helper: construct a callable from a set of lambdas.
///
///   std::visit(overloaded{
///       [](const Foo &f) { ... },
///       [](const Bar &b) { ... },
///       [](const auto &)  { ... },  // catch-all
///   }, variant);
template <typename... Ts> struct overloaded : Ts... {
    using Ts::operator()...;
};
template <typename... Ts> overloaded(Ts...) -> overloaded<Ts...>;

} // namespace nodehammer::detail
