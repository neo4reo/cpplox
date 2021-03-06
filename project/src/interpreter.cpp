// Related header
#include "interpreter.hpp"
// C standard headers
#include <cstddef>
// C++ standard headers
#include <algorithm>
#include <chrono>
#include <iostream>
#include <iterator>
#include <string>
#include <unordered_map>
#include <utility>
// Third-party headers
#include <boost/variant.hpp>
#include <gsl/gsl_util>
// This project's headers
#include "callable.hpp"
#include "literal.hpp"
#include "utility.hpp"

using std::back_inserter;
using std::chrono::duration_cast;
using std::chrono::seconds;
using std::chrono::system_clock;
using std::cout;
using std::find_if;
using std::make_pair;
using std::make_shared;
using std::move;
using std::nullptr_t;
using std::pair;
using std::shared_ptr;
using std::string;
using std::to_string;
using std::transform;
using std::unordered_map;
using std::vector;

using boost::bad_get;
using boost::get;
using boost::static_visitor;
using gsl::finally;
using gsl::narrow;

// For the occasional explicit qualification, mostly just to aid us human readers
namespace lox = motts::lox;

namespace motts { namespace lox {
    class Environment {
        // This is conceptually an unordered map, but prefer vector as default container
        vector<pair<string, Literal>> values_;

        shared_ptr<Environment> enclosed_;

        public:
            explicit Environment() = default;

            explicit Environment(shared_ptr<Environment> enclosed) :
                enclosed_ {move(enclosed)}
            {}

            vector<pair<string, Literal>>::iterator find_in_chain(const string& var_name) {
                const auto found_own = find_if(values_.begin(), values_.end(), [&] (const auto& value) {
                    return value.first == var_name;
                });
                if (found_own != end()) return found_own;

                if (enclosed_) {
                    const auto found_enclosed = enclosed_->find_in_chain(var_name);
                    if (found_enclosed != enclosed_->end()) {
                        return found_enclosed;
                    }
                }

                return end();
            }

            Literal& find_own_or_make(const string& var_name) {
                const auto found_own = find_if(values_.begin(), values_.end(), [&] (const auto& value) {
                    return value.first == var_name;
                });
                if (found_own != end()) return found_own->second;

                values_.push_back(make_pair(var_name, Literal{}));

                return values_.back().second;
            }

            vector<pair<string, Literal>>::iterator end() {
                return values_.end();
            }
    };

    class Interpreter::Function : public Callable {
        std::shared_ptr<Function_stmt> declaration_;
        std::shared_ptr<Environment> enclosed_;

        public:
            explicit Function(shared_ptr<Function_stmt> declaration, shared_ptr<Environment> enclosed) :
                declaration_ {declaration},
                enclosed_ {enclosed}
            {}

            Literal call(Interpreter& interpreter, const vector<Literal>& arguments) const override {
                auto caller_environment = move(interpreter.environment_);
                interpreter.environment_ = make_shared<Environment>(enclosed_);
                const auto _ = finally([&] () {
                    interpreter.environment_ = move(caller_environment);
                });

                for (decltype(declaration_->parameters.size()) i {0}; i < declaration_->parameters.size(); ++i) {
                    interpreter.environment_->find_own_or_make(declaration_->parameters.at(i).lexeme) = arguments.at(i);
                }

                for (const auto& statement : declaration_->body) {
                    statement->accept(interpreter);

                    if (interpreter.returning_) {
                        interpreter.returning_ = false;
                        return move(interpreter.result_);
                    }
                }

                return Literal{};
            }

            int arity() const override {
                return narrow<int>(declaration_->parameters.size());
            }

            string to_string() const override {
                return "<fn " + declaration_->name.lexeme + ">";
            }
    };

    Interpreter::Interpreter() :
        environment_ {make_shared<Environment>()},
        globals_ {environment_}
    {
        struct Clock_callable : Callable {
            Literal call(Interpreter&, const vector<Literal>& /*arguments*/) const override {
                return Literal{narrow<double>(duration_cast<seconds>(system_clock::now().time_since_epoch()).count())};
            }

            int arity() const override {
                return 0;
            }

            string to_string() const override {
                return "<fn clock>";
            }
        };

        globals_->find_own_or_make("clock") = Literal{make_shared<Clock_callable>()};
    }

    void Interpreter::visit(const Literal_expr& expr) {
        result_ = expr.value;
    }

    void Interpreter::visit(const Grouping_expr& expr) {
        expr.expr->accept(*this);
    }

    // False and nil are falsey, everything else is truthy
    struct Is_truthy_visitor : static_visitor<bool> {
        auto operator()(bool value) const {
            return value;
        }

        auto operator()(nullptr_t) const {
            return false;
        }

        template<typename T>
            auto operator()(const T&) const {
                return true;
            }
    };

    void Interpreter::visit(const Unary_expr& expr) {
        const auto unary_result = lox::apply_visitor(*this, *(expr.right));

        try {
            switch (expr.op.type) {
                case Token_type::minus:
                    result_ = Literal{ - get<double>(unary_result.value)};
                    break;

                case Token_type::bang:
                    result_ = Literal{ ! boost::apply_visitor(Is_truthy_visitor{}, unary_result.value)};
                    break;

                default:
                    throw Interpreter_error{"Unreachable.", expr.op};
            }
        } catch (const bad_get&) {
            // Convert a boost variant error into a Lox error
            throw Interpreter_error{"Operands must be numbers.", expr.op};
        }
    }

    struct Plus_visitor : static_visitor<Literal> {
        auto operator()(const string& lhs, const string& rhs) const {
            return Literal{lhs + rhs};
        }

        auto operator()(double lhs, double rhs) const {
            return Literal{lhs + rhs};
        }

        // All other type combinations can't be '+'-ed together
        template<typename T, typename U>
            Literal operator()(const T&, const U&) const {
                throw Interpreter_error{"Operands must be two numbers or two strings."};
            }
    };

    struct Is_equal_visitor : static_visitor<bool> {
        // Different types automatically compare false
        template<typename T, typename U>
            auto operator()(const T&, const U&) const {
                return false;
            }

        // Same types use normal equality test
        template<typename T>
            auto operator()(const T& lhs, const T& rhs) const {
                return lhs == rhs;
            }
    };

    void Interpreter::visit(const Binary_expr& expr) {
        const auto left_result = lox::apply_visitor(*this, *(expr.left));
        const auto right_result = lox::apply_visitor(*this, *(expr.right));

        try {
            switch (expr.op.type) {
                case Token_type::greater:
                    result_ = Literal{get<double>(left_result.value) > get<double>(right_result.value)};
                    break;

                case Token_type::greater_equal:
                    result_ = Literal{get<double>(left_result.value) >= get<double>(right_result.value)};
                    break;

                case Token_type::less:
                    result_ = Literal{get<double>(left_result.value) < get<double>(right_result.value)};
                    break;

                case Token_type::less_equal:
                    result_ = Literal{get<double>(left_result.value) <= get<double>(right_result.value)};
                    break;

                case Token_type::bang_equal:
                    result_ = Literal{
                        ! boost::apply_visitor(Is_equal_visitor{}, left_result.value, right_result.value)
                    };
                    break;

                case Token_type::equal_equal:
                    result_ = Literal{boost::apply_visitor(Is_equal_visitor{}, left_result.value, right_result.value)};
                    break;

                case Token_type::minus:
                    result_ = Literal{get<double>(left_result.value) - get<double>(right_result.value)};
                    break;

                case Token_type::plus:
                    result_ = boost::apply_visitor(Plus_visitor{}, left_result.value, right_result.value);
                    break;

                case Token_type::slash:
                    result_ = Literal{get<double>(left_result.value) / get<double>(right_result.value)};
                    break;

                case Token_type::star:
                    result_ = Literal{get<double>(left_result.value) * get<double>(right_result.value)};
                    break;

                default:
                    throw Interpreter_error{"Unreachable.", expr.op};
            }
        } catch (const bad_get&) {
            // Convert a boost variant error into a Lox error
            throw Interpreter_error{"Operands must be numbers.", expr.op};
        }
    }

    void Interpreter::visit(const Var_expr& expr) {
        const auto found = environment_->find_in_chain(expr.name.lexeme);
        if (found == environment_->end()) {
            throw Interpreter_error{"Undefined variable '" + expr.name.lexeme + "'."};
        }

        result_ = found->second;
    }

    void Interpreter::visit(const Assign_expr& expr) {
        const auto found = environment_->find_in_chain(expr.name.lexeme);
        if (found == environment_->end()) {
            throw Interpreter_error{"Undefined variable '" + expr.name.lexeme + "'."};
        }

        result_ = found->second = lox::apply_visitor(*this, *(expr.value));
    }

    void Interpreter::visit(const Logical_expr& expr) {
        auto left_result = lox::apply_visitor(*this, *(expr.left));

        // Short circuit if possible
        if (expr.op.type == Token_type::or_) {
            if (boost::apply_visitor(Is_truthy_visitor{}, left_result.value)) {
                result_ = move(left_result);
                return;
            }
        } else if (expr.op.type == Token_type::and_) {
            if ( ! boost::apply_visitor(Is_truthy_visitor{}, left_result.value)) {
                result_ = move(left_result);
                return;
            }
        } else {
            throw Interpreter_error{"Unreachable.", expr.op};
        }

        expr.right->accept(*this);
    }

    void Interpreter::visit(const Call_expr& expr) {
        auto callee_result = apply_visitor(*this, *(expr.callee));
        const auto callable = ([&] () {
            try {
                return get<shared_ptr<Callable>>(move(callee_result).value);
            } catch (const bad_get&) {
                // Convert a boost variant error into a Lox error
                throw Interpreter_error{"Can only call functions and classes."};
            }
        })();

        if (narrow<int>(expr.arguments.size()) != callable->arity()) {
            throw Interpreter_error{
                "Expected " + to_string(callable->arity()) +
                " arguments but got " + to_string(expr.arguments.size()) + ".",
                expr.closing_paren
            };
        }

        vector<Literal> arguments_results;
        transform(
            expr.arguments.cbegin(), expr.arguments.cend(), back_inserter(arguments_results),
            [&] (const auto& argument_expr) {
                return apply_visitor(*this, *argument_expr);
            }
        );

        result_ = callable->call(*this, arguments_results);
    }

    void Interpreter::visit(const Expr_stmt& stmt) {
        stmt.expr->accept(*this);
    }

    void Interpreter::visit(const If_stmt& if_stmt) {
        const auto condition_result = lox::apply_visitor(*this, *(if_stmt.condition));
        if (boost::apply_visitor(Is_truthy_visitor{}, condition_result.value)) {
            if_stmt.then_branch->accept(*this);

            if (returning_) {
                return;
            }
        } else if (if_stmt.else_branch) {
            if_stmt.else_branch->accept(*this);
        }
    }

    void Interpreter::visit(const While_stmt& stmt) {
        while (
            // IIFE so I can execute multiple statements inside while condition
            ([&] () {
                const auto condition_result = lox::apply_visitor(*this, *(stmt.condition));
                return boost::apply_visitor(Is_truthy_visitor{}, condition_result.value);
            })()
        ) {
            stmt.body->accept(*this);

            if (returning_) {
                return;
            }
        }
    }

    void Interpreter::visit(const Print_stmt& stmt) {
        cout << lox::apply_visitor(*this, *(stmt.expr)) << "\n";
    }

    void Interpreter::visit(const Var_stmt& stmt) {
        environment_->find_own_or_make(stmt.name.lexeme) = (
            stmt.initializer ?
                lox::apply_visitor(*this, *(stmt.initializer)) :
                Literal{}
        );
    }

    void Interpreter::visit(const Block_stmt& stmt) {
        auto enclosed = move(environment_);
        environment_ = make_shared<Environment>(enclosed);
        const auto _ = finally([&] () {
            environment_ = move(enclosed);
        });

        for (const auto& statement : stmt.statements) {
            statement->accept(*this);

            if (returning_) {
                return;
            }
        }
    }

    void Interpreter::visit(const Function_stmt& stmt) {
        const auto name = stmt.name.lexeme;
        environment_->find_own_or_make(name) = Literal{
            make_shared<Function>(make_shared<Function_stmt>(Function_stmt{stmt}), environment_)
        };
    }

    void Interpreter::visit(const Return_stmt& stmt) {
        Literal value;
        if (stmt.value) {
            value = apply_visitor(*this, *(stmt.value));
        }

        result_ = move(value);
        returning_ = true;
    }

    const Literal& Interpreter::result() const & {
        return result_;
    }

    Literal&& Interpreter::result() && {
        return move(result_);
    }

    Interpreter_error::Interpreter_error(const string& what, const Token& token) :
        Runtime_error {
            "[Line " + to_string(token.line) + "] Error '" + token.lexeme + "': " + what
        }
    {}
}}
