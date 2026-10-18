// SPDX-FileCopyrightText: 2026 StillJC
// SPDX-License-Identifier: GPL-3.0-only

#include "mame_cheat_expression.h"

#include "common/error.h"

#include <array>
#include <cctype>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

namespace MAMECheats {
namespace {

enum class ErrorCode : u8
{
  NotLValue,
  NotRValue,
  Syntax,
  UnknownSymbol,
  InvalidNumber,
  InvalidToken,
  UnbalancedParentheses,
  DivideByZero,
  InvalidParameterCount,
  TooFewParameters,
  TooManyParameters,
  UnbalancedQuotes,
  InvalidMemorySize,
  InvalidMemorySpace,
  InvalidMemoryName,
  UnsupportedMemory,
};

struct ExpressionErrorInfo
{
  ErrorCode code = ErrorCode::Syntax;
  size_t offset = 0;
  u32 parameter = 0;
  bool valid = false;
};

void SetInternalError(ExpressionErrorInfo* error, ErrorCode code, size_t offset, u32 parameter = 0)
{
  if (!error || error->valid)
    return;

  error->code = code;
  error->offset = offset;
  error->parameter = parameter;
  error->valid = true;
}

const char* GetErrorText(ErrorCode code)
{
  switch (code)
  {
    case ErrorCode::NotLValue:
      return "not an lvalue";
    case ErrorCode::NotRValue:
      return "not an rvalue";
    case ErrorCode::Syntax:
      return "syntax error";
    case ErrorCode::UnknownSymbol:
      return "unknown symbol";
    case ErrorCode::InvalidNumber:
      return "invalid number";
    case ErrorCode::InvalidToken:
      return "invalid token";
    case ErrorCode::UnbalancedParentheses:
      return "unbalanced parentheses";
    case ErrorCode::DivideByZero:
      return "divide by zero";
    case ErrorCode::InvalidParameterCount:
      return "invalid number of parameters";
    case ErrorCode::TooFewParameters:
      return "too few parameters";
    case ErrorCode::TooManyParameters:
      return "too many parameters";
    case ErrorCode::UnbalancedQuotes:
      return "unbalanced quotes";
    case ErrorCode::InvalidMemorySize:
      return "invalid memory size";
    case ErrorCode::InvalidMemorySpace:
      return "invalid memory space";
    case ErrorCode::InvalidMemoryName:
      return "invalid memory name";
    case ErrorCode::UnsupportedMemory:
      return "unsupported memory source or address space";
    default:
      return "expression error";
  }
}

void SetExpressionError(Error* error, const ExpressionErrorInfo& expression_error)
{
  if (expression_error.code == ErrorCode::TooFewParameters)
  {
    Error::SetStringFmt(error, "Expression error at offset {}: too few parameters (at least {} required)",
                        expression_error.offset, expression_error.parameter);
  }
  else if (expression_error.code == ErrorCode::TooManyParameters)
  {
    Error::SetStringFmt(error, "Expression error at offset {}: too many parameters (no more than {} accepted)",
                        expression_error.offset, expression_error.parameter);
  }
  else
  {
    Error::SetStringFmt(error, "Expression error at offset {}: {}", expression_error.offset,
                        GetErrorText(expression_error.code));
  }
}

std::string Lowercase(std::string_view value)
{
  std::string result(value);
  for (char& ch : result)
    ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
  return result;
}

bool IsWordCharacter(char ch)
{
  const unsigned char uch = static_cast<unsigned char>(ch);
  return std::isalnum(uch) != 0 || ch == '_' || ch == '$' || ch == '#' || ch == '.' || ch == ':';
}

enum class SymbolType : u8
{
  Value,
  Function,
};

struct Symbol
{
  SymbolType type = SymbolType::Value;
  ExpressionSymbolTable::Getter getter;
  ExpressionSymbolTable::Setter setter;
  ExpressionSymbolTable::Function function;
  u32 minimum_parameters = 0;
  u32 maximum_parameters = 0;
};

enum class TokenType : u8
{
  End,
  Number,
  String,
  Symbol,
  Function,
  Memory,
  LeftParen,
  RightParen,
  Comma,
  PrefixIncrement,
  PrefixDecrement,
  PostfixIncrement,
  PostfixDecrement,
  LogicalNot,
  BitwiseNot,
  UnaryPlus,
  UnaryMinus,
  Multiply,
  Divide,
  Modulo,
  Add,
  Subtract,
  ShiftLeft,
  ShiftRight,
  Less,
  LessEqual,
  Greater,
  GreaterEqual,
  Equal,
  NotEqual,
  BitwiseAnd,
  BitwiseXor,
  BitwiseOr,
  LogicalAnd,
  LogicalAndStrong,
  LogicalOr,
  Assign,
  AssignMultiply,
  AssignDivide,
  AssignModulo,
  AssignAdd,
  AssignSubtract,
  AssignShiftLeft,
  AssignShiftRight,
  AssignAnd,
  AssignXor,
  AssignOr,
};

// Memory tokens carry the MAME debugger access space and width through parsing so lvalue writes use
// the same addressing semantics as reads.
struct MemorySpec
{
  std::string name;
  ExpressionMemorySpace space = ExpressionMemorySpace::ProgramLogical;
  u32 size = 1;
  bool disable_side_effects = true;
};

struct Token
{
  TokenType type = TokenType::End;
  size_t offset = 0;
  u64 number = 0;
  std::string text;
  const Symbol* symbol = nullptr;
  MemorySpec memory;
};

bool ParseNumber(std::string_view text, int base, u64* value)
{
  if (text.empty())
    return false;

  u64 parsed = 0;
  for (const char ch : text)
  {
    const unsigned char uch = static_cast<unsigned char>(ch);
    const char lower = static_cast<char>(std::tolower(uch));
    u32 digit;
    if (lower >= '0' && lower <= '9')
      digit = static_cast<u32>(lower - '0');
    else if (lower >= 'a' && lower <= 'f')
      digit = 10u + static_cast<u32>(lower - 'a');
    else
      return false;

    if (digit >= static_cast<u32>(base))
      return false;

    parsed = (parsed * static_cast<u64>(base)) + digit;
  }

  *value = parsed;
  return true;
}

class Lexer
{
public:
  Lexer(std::string_view expression, const std::unordered_map<std::string, Symbol>& symbols,
        const ExpressionMemoryAccess* memory, ExpressionErrorInfo* error)
    : m_expression(expression), m_symbols(symbols), m_memory(memory), m_error(error)
  {
  }

  Token Next()
  {
    SkipWhitespace();
    Token token;
    token.offset = m_position;
    if (m_position >= m_expression.size())
      return token;

    const char ch = m_expression[m_position];
    switch (ch)
    {
      case '(':
        m_position++;
        token.type = TokenType::LeftParen;
        return token;
      case ')':
        m_position++;
        token.type = TokenType::RightParen;
        return token;
      case ',':
        m_position++;
        token.type = TokenType::Comma;
        return token;
      case '~':
        m_position++;
        token.type = TokenType::BitwiseNot;
        return token;
      case '+':
        return LexTwoCharacterOperator(token, '+', TokenType::PrefixIncrement, '=', TokenType::AssignAdd,
                                       TokenType::Add);
      case '-':
        return LexTwoCharacterOperator(token, '-', TokenType::PrefixDecrement, '=', TokenType::AssignSubtract,
                                       TokenType::Subtract);
      case '*':
        return LexAssignmentOperator(token, TokenType::Multiply, TokenType::AssignMultiply);
      case '/':
        return LexAssignmentOperator(token, TokenType::Divide, TokenType::AssignDivide);
      case '%':
        return LexAssignmentOperator(token, TokenType::Modulo, TokenType::AssignModulo);
      case '&':
        if (Peek(1) == '&')
        {
          m_position += 2;
          token.type = TokenType::LogicalAnd;
        }
        else
        {
          token = LexAssignmentOperator(token, TokenType::BitwiseAnd, TokenType::AssignAnd);
        }
        return token;
      case '|':
        if (Peek(1) == '|')
        {
          m_position += 2;
          token.type = TokenType::LogicalOr;
        }
        else
        {
          token = LexAssignmentOperator(token, TokenType::BitwiseOr, TokenType::AssignOr);
        }
        return token;
      case '^':
        return LexAssignmentOperator(token, TokenType::BitwiseXor, TokenType::AssignXor);
      case '=':
        m_position++;
        if (Peek(0) == '=')
        {
          m_position++;
          token.type = TokenType::Equal;
        }
        else
        {
          token.type = TokenType::Assign;
        }
        return token;
      case '!':
        m_position++;
        if (Peek(0) == '=')
        {
          m_position++;
          token.type = TokenType::NotEqual;
        }
        else
        {
          token.type = TokenType::LogicalNot;
        }
        return token;
      case '<':
        m_position++;
        if (Peek(0) == '<')
        {
          m_position++;
          if (Peek(0) == '=')
          {
            m_position++;
            token.type = TokenType::AssignShiftLeft;
          }
          else
          {
            token.type = TokenType::ShiftLeft;
          }
        }
        else if (Peek(0) == '=')
        {
          m_position++;
          token.type = TokenType::LessEqual;
        }
        else
        {
          token.type = TokenType::Less;
        }
        return token;
      case '>':
        m_position++;
        if (Peek(0) == '>')
        {
          m_position++;
          if (Peek(0) == '=')
          {
            m_position++;
            token.type = TokenType::AssignShiftRight;
          }
          else
          {
            token.type = TokenType::ShiftRight;
          }
        }
        else if (Peek(0) == '=')
        {
          m_position++;
          token.type = TokenType::GreaterEqual;
        }
        else
        {
          token.type = TokenType::Greater;
        }
        return token;
      case '\'':
        return LexQuotedCharacter(token);
      case '"':
        return LexQuotedString(token);
      default:
        return LexWord(token);
    }
  }

private:
  char Peek(size_t distance) const
  {
    const size_t position = m_position + distance;
    return (position < m_expression.size()) ? m_expression[position] : '\0';
  }

  void SkipWhitespace()
  {
    while (m_position < m_expression.size() &&
           std::isspace(static_cast<unsigned char>(m_expression[m_position])) != 0)
    {
      m_position++;
    }
  }

  Token LexTwoCharacterOperator(Token token, char repeated, TokenType repeated_type, char assignment,
                                TokenType assignment_type, TokenType single_type)
  {
    m_position++;
    if (Peek(0) == repeated)
    {
      m_position++;
      token.type = repeated_type;
    }
    else if (Peek(0) == assignment)
    {
      m_position++;
      token.type = assignment_type;
    }
    else
    {
      token.type = single_type;
    }
    return token;
  }

  Token LexAssignmentOperator(Token token, TokenType plain_type, TokenType assignment_type)
  {
    m_position++;
    if (Peek(0) == '=')
    {
      m_position++;
      token.type = assignment_type;
    }
    else
    {
      token.type = plain_type;
    }
    return token;
  }

  Token LexQuotedCharacter(Token token)
  {
    m_position++;
    u64 value = 0;
    bool closed = false;
    while (m_position < m_expression.size())
    {
      if (m_expression[m_position] == '\'')
      {
        if ((m_position + 1) < m_expression.size() && m_expression[m_position + 1] == '\'')
        {
          m_position++;
        }
        else
        {
          m_position++;
          closed = true;
          break;
        }
      }

      value = (value << 8) | static_cast<u8>(m_expression[m_position++]);
    }

    if (!closed)
    {
      SetInternalError(m_error, ErrorCode::UnbalancedQuotes, token.offset);
      return token;
    }

    token.type = TokenType::Number;
    token.number = value;
    return token;
  }

  Token LexQuotedString(Token token)
  {
    m_position++;
    std::string value;
    bool closed = false;
    while (m_position < m_expression.size())
    {
      if (m_expression[m_position] == '"')
      {
        if ((m_position + 1) < m_expression.size() && m_expression[m_position + 1] == '"')
        {
          m_position++;
        }
        else
        {
          m_position++;
          closed = true;
          break;
        }
      }

      value.push_back(m_expression[m_position++]);
    }

    if (!closed)
    {
      SetInternalError(m_error, ErrorCode::UnbalancedQuotes, token.offset);
      return token;
    }

    token.type = TokenType::String;
    token.text = std::move(value);
    return token;
  }

  Token LexWord(Token token)
  {
    const size_t start = m_position;
    while (m_position < m_expression.size() && IsWordCharacter(m_expression[m_position]))
      m_position++;

    if (m_position == start)
    {
      SetInternalError(m_error, ErrorCode::InvalidToken, start);
      return token;
    }

    std::string word = Lowercase(m_expression.substr(start, m_position - start));
    if (Peek(0) == '@' || Peek(0) == '!')
    {
      ExpressionErrorInfo memory_error;
      MemorySpec memory_spec = ParseMemorySpec(word, Peek(0) == '@', start, &memory_error);
      if (!memory_error.valid)
      {
        token.type = TokenType::Memory;
        token.memory = std::move(memory_spec);
        m_position++;
        return token;
      }

      if (memory_error.code == ErrorCode::UnsupportedMemory)
      {
        SetInternalError(m_error, memory_error.code, memory_error.offset, memory_error.parameter);
        return token;
      }
    }

    if (word == "bnot")
      token.type = TokenType::BitwiseNot;
    else if (word == "plus")
      token.type = TokenType::Add;
    else if (word == "minus")
      token.type = TokenType::Subtract;
    else if (word == "times" || word == "mul")
      token.type = TokenType::Multiply;
    else if (word == "div")
      token.type = TokenType::Divide;
    else if (word == "mod")
      token.type = TokenType::Modulo;
    else if (word == "lt")
      token.type = TokenType::Less;
    else if (word == "le")
      token.type = TokenType::LessEqual;
    else if (word == "gt")
      token.type = TokenType::Greater;
    else if (word == "ge")
      token.type = TokenType::GreaterEqual;
    else if (word == "eq")
      token.type = TokenType::Equal;
    else if (word == "ne")
      token.type = TokenType::NotEqual;
    else if (word == "not")
      token.type = TokenType::LogicalNot;
    else if (word == "and")
      token.type = TokenType::LogicalAndStrong;
    else if (word == "band")
      token.type = TokenType::BitwiseAnd;
    else if (word == "or")
      token.type = TokenType::LogicalOr;
    else if (word == "bor")
      token.type = TokenType::BitwiseOr;
    else if (word == "bxor")
      token.type = TokenType::BitwiseXor;
    else if (word == "lshift")
      token.type = TokenType::ShiftLeft;
    else if (word == "rshift")
      token.type = TokenType::ShiftRight;
    else
      return LexSymbolOrNumber(token, word);

    return token;
  }

  Token LexSymbolOrNumber(Token token, const std::string& word)
  {
    const auto symbol_it = m_symbols.find(word);
    if (symbol_it != m_symbols.end())
    {
      token.type = (symbol_it->second.type == SymbolType::Function) ? TokenType::Function : TokenType::Symbol;
      token.symbol = &symbol_it->second;
      token.text = word;
      return token;
    }

    u64 value = 0;
    if (word[0] == '#')
    {
      if (!ParseNumber(std::string_view(word).substr(1), 10, &value))
      {
        SetInternalError(m_error, ErrorCode::InvalidNumber, token.offset);
        return token;
      }
    }
    else if (word[0] == '$')
    {
      if (!ParseNumber(std::string_view(word).substr(1), 16, &value))
      {
        SetInternalError(m_error, ErrorCode::InvalidNumber, token.offset);
        return token;
      }
    }
    else if (word.size() > 1 && word[0] == '0' && word[1] == 'x')
    {
      if (!ParseNumber(std::string_view(word).substr(2), 16, &value))
      {
        SetInternalError(m_error, ErrorCode::InvalidNumber, token.offset);
        return token;
      }
    }
    else if (word.size() > 1 && word[0] == '0' && word[1] == 'o')
    {
      if (!ParseNumber(std::string_view(word).substr(2), 8, &value))
      {
        SetInternalError(m_error, ErrorCode::InvalidNumber, token.offset);
        return token;
      }
    }
    else if (word.size() > 1 && word[0] == '0' && word[1] == 'b')
    {
      if (!ParseNumber(std::string_view(word).substr(2), 2, &value) && !ParseNumber(word, 16, &value))
      {
        SetInternalError(m_error, ErrorCode::InvalidNumber, token.offset);
        return token;
      }
    }
    else if (!ParseNumber(word, 16, &value))
    {
      SetInternalError(m_error, ErrorCode::UnknownSymbol, token.offset);
      return token;
    }

    token.type = TokenType::Number;
    token.number = value;
    return token;
  }

  MemorySpec ParseMemorySpec(std::string_view text, bool disable_side_effects, size_t offset,
                             ExpressionErrorInfo* error) const
  {
    MemorySpec spec;
    spec.disable_side_effects = disable_side_effects;

    std::string_view suffix = text;
    const size_t dot = text.find_last_of('.');
    if (dot != std::string_view::npos)
    {
      spec.name.assign(text.substr(0, dot));
      suffix = text.substr(dot + 1);
    }

    bool physical = false;
    char space = 'p';
    char size = '\0';
    if (suffix.size() == 3)
    {
      if (suffix[0] != 'l' && suffix[0] != 'p')
      {
        SetInternalError(error, ErrorCode::InvalidMemorySpace, offset);
        return spec;
      }
      if (suffix[1] != 'p' && suffix[1] != 'd' && suffix[1] != 'i' && suffix[1] != '3')
      {
        SetInternalError(error, ErrorCode::InvalidMemorySpace, offset);
        return spec;
      }
      physical = (suffix[0] == 'p');
      space = suffix[1];
      size = suffix[2];
    }
    else if (suffix.size() == 2)
    {
      space = suffix[0];
      size = suffix[1];
    }
    else if (suffix.size() == 1)
    {
      size = suffix[0];
    }
    else
    {
      SetInternalError(error, ErrorCode::InvalidToken, offset);
      return spec;
    }

    switch (space)
    {
      case 'p':
        spec.space = physical ? ExpressionMemorySpace::ProgramPhysical : ExpressionMemorySpace::ProgramLogical;
        break;
      case 'd':
        spec.space = physical ? ExpressionMemorySpace::DataPhysical : ExpressionMemorySpace::DataLogical;
        break;
      case 'i':
        spec.space = physical ? ExpressionMemorySpace::IOPhysical : ExpressionMemorySpace::IOLogical;
        break;
      case '3':
        spec.space = physical ? ExpressionMemorySpace::OpcodePhysical : ExpressionMemorySpace::OpcodeLogical;
        break;
      case 'r':
        spec.space = ExpressionMemorySpace::ProgramDirect;
        break;
      case 'o':
        spec.space = ExpressionMemorySpace::OpcodeDirect;
        break;
      case 'm':
        spec.space = ExpressionMemorySpace::Region;
        break;
      case 's':
        spec.space = ExpressionMemorySpace::Share;
        break;
      default:
        SetInternalError(error, ErrorCode::InvalidMemorySpace, offset);
        return spec;
    }

    switch (size)
    {
      case 'b':
        spec.size = 1;
        break;
      case 'w':
        spec.size = 2;
        break;
      case 'd':
        spec.size = 4;
        break;
      case 'q':
        spec.size = 8;
        break;
      default:
        SetInternalError(error, ErrorCode::InvalidMemorySize, offset);
        return spec;
    }

    if (spec.name.empty())
    {
      SetInternalError(error, ErrorCode::InvalidMemoryName, offset);
      return spec;
    }
    if (!m_memory || !m_memory->IsValid(spec.name, spec.space))
    {
      SetInternalError(error, ErrorCode::UnsupportedMemory, offset);
      return spec;
    }

    return spec;
  }

  std::string_view m_expression;
  const std::unordered_map<std::string, Symbol>& m_symbols;
  const ExpressionMemoryAccess* m_memory;
  ExpressionErrorInfo* m_error;
  size_t m_position = 0;
};

enum class NodeType : u8
{
  Number,
  String,
  Symbol,
  Memory,
  Function,
  Unary,
  Binary,
  PrefixIncrement,
  PrefixDecrement,
  PostfixIncrement,
  PostfixDecrement,
  Assignment,
};

struct ResolvedLValue
{
  const Symbol* symbol = nullptr;
  const ExpressionMemoryAccess* memory_access = nullptr;
  MemorySpec memory;
  u32 address = 0;
  size_t offset = 0;

  u64 Read(ExpressionErrorInfo* error) const
  {
    if (symbol)
      return symbol->getter();
    if (memory_access)
      return memory_access->Read(memory.name, memory.space, address, memory.size, memory.disable_side_effects);

    SetInternalError(error, ErrorCode::NotLValue, offset);
    return 0;
  }

  void Write(u64 value, ExpressionErrorInfo* error) const
  {
    if (symbol && symbol->setter)
    {
      symbol->setter(value);
      return;
    }
    if (memory_access)
    {
      memory_access->Write(memory.name, memory.space, address, memory.size, value, memory.disable_side_effects);
      return;
    }

    SetInternalError(error, ErrorCode::NotLValue, offset);
  }
};

struct Node
{
  NodeType type = NodeType::Number;
  TokenType operation = TokenType::End;
  size_t offset = 0;
  u64 number = 0;
  std::string text;
  const Symbol* symbol = nullptr;
  MemorySpec memory;
  std::unique_ptr<Node> left;
  std::unique_ptr<Node> right;
  std::vector<std::unique_ptr<Node>> arguments;

  ResolvedLValue ResolveLValue(const ExpressionMemoryAccess* memory_access, ExpressionErrorInfo* error) const
  {
    ResolvedLValue result;
    result.offset = offset;
    if (type == NodeType::Symbol && symbol && symbol->setter)
    {
      result.symbol = symbol;
      return result;
    }
    if (type == NodeType::Memory && left && memory_access)
    {
      result.memory_access = memory_access;
      result.memory = memory;
      result.address = static_cast<u32>(left->Evaluate(memory_access, error));
      return result;
    }

    SetInternalError(error, ErrorCode::NotLValue, offset);
    return result;
  }

  u64 Evaluate(const ExpressionMemoryAccess* memory_access, ExpressionErrorInfo* error) const
  {
    if (error && error->valid)
      return 0;

    switch (type)
    {
      case NodeType::Number:
        return number;
      case NodeType::String:
        SetInternalError(error, ErrorCode::NotRValue, offset);
        return 0;
      case NodeType::Symbol:
        return symbol->getter();
      case NodeType::Memory:
      {
        const ResolvedLValue target = ResolveLValue(memory_access, error);
        return (error && error->valid) ? 0 : target.Read(error);
      }
      case NodeType::Function:
      {
        if (!symbol || symbol->type != SymbolType::Function)
        {
          SetInternalError(error, ErrorCode::Syntax, offset);
          return 0;
        }
        if (arguments.size() < symbol->minimum_parameters)
        {
          SetInternalError(error, ErrorCode::TooFewParameters, offset, symbol->minimum_parameters);
          return 0;
        }
        if (arguments.size() > symbol->maximum_parameters)
        {
          SetInternalError(error, ErrorCode::TooManyParameters, offset, symbol->maximum_parameters);
          return 0;
        }

        std::array<u64, 16> values{};
        if (arguments.size() > values.size())
        {
          SetInternalError(error, ErrorCode::InvalidParameterCount, offset);
          return 0;
        }
        for (size_t i = 0; i < arguments.size(); i++)
        {
          values[i] = arguments[i]->Evaluate(memory_access, error);
          if (error && error->valid)
            return 0;
        }
        return symbol->function(static_cast<u32>(arguments.size()), values.data());
      }
      case NodeType::PrefixIncrement:
      {
        const ResolvedLValue target = left->ResolveLValue(memory_access, error);
        if (error && error->valid)
          return 0;
        const u64 value = target.Read(error) + 1;
        if (error && error->valid)
          return 0;
        target.Write(value, error);
        return (error && error->valid) ? 0 : value;
      }
      case NodeType::PrefixDecrement:
      {
        const ResolvedLValue target = left->ResolveLValue(memory_access, error);
        if (error && error->valid)
          return 0;
        const u64 value = target.Read(error) - 1;
        if (error && error->valid)
          return 0;
        target.Write(value, error);
        return (error && error->valid) ? 0 : value;
      }
      case NodeType::PostfixIncrement:
      {
        const ResolvedLValue target = left->ResolveLValue(memory_access, error);
        if (error && error->valid)
          return 0;
        const u64 value = target.Read(error);
        if (error && error->valid)
          return 0;
        target.Write(value + 1, error);
        return (error && error->valid) ? 0 : value;
      }
      case NodeType::PostfixDecrement:
      {
        const ResolvedLValue target = left->ResolveLValue(memory_access, error);
        if (error && error->valid)
          return 0;
        const u64 value = target.Read(error);
        if (error && error->valid)
          return 0;
        target.Write(value - 1, error);
        return (error && error->valid) ? 0 : value;
      }
      case NodeType::Unary:
      {
        const u64 value = left->Evaluate(memory_access, error);
        if (error && error->valid)
          return 0;
        switch (operation)
        {
          case TokenType::LogicalNot:
            return !value;
          case TokenType::BitwiseNot:
            return ~value;
          case TokenType::UnaryPlus:
            return value;
          case TokenType::UnaryMinus:
            return 0 - value;
          default:
            SetInternalError(error, ErrorCode::Syntax, offset);
            return 0;
        }
      }
      case NodeType::Binary:
      {
        const u64 lhs = left->Evaluate(memory_access, error);
        if (error && error->valid)
          return 0;
        const u64 rhs = right->Evaluate(memory_access, error);
        if (error && error->valid)
          return 0;
        switch (operation)
        {
          case TokenType::Multiply:
            return lhs * rhs;
          case TokenType::Divide:
            if (rhs == 0)
            {
              SetInternalError(error, ErrorCode::DivideByZero, right->offset);
              return 0;
            }
            return lhs / rhs;
          case TokenType::Modulo:
            if (rhs == 0)
            {
              SetInternalError(error, ErrorCode::DivideByZero, right->offset);
              return 0;
            }
            return lhs % rhs;
          case TokenType::Add:
            return lhs + rhs;
          case TokenType::Subtract:
            return lhs - rhs;
          case TokenType::ShiftLeft:
            return (rhs < 64) ? (lhs << rhs) : 0;
          case TokenType::ShiftRight:
            return (rhs < 64) ? (lhs >> rhs) : 0;
          case TokenType::Less:
            return lhs < rhs;
          case TokenType::LessEqual:
            return lhs <= rhs;
          case TokenType::Greater:
            return lhs > rhs;
          case TokenType::GreaterEqual:
            return lhs >= rhs;
          case TokenType::Equal:
            return lhs == rhs;
          case TokenType::NotEqual:
            return lhs != rhs;
          case TokenType::BitwiseAnd:
            return lhs & rhs;
          case TokenType::BitwiseXor:
            return lhs ^ rhs;
          case TokenType::BitwiseOr:
            return lhs | rhs;
          case TokenType::LogicalAnd:
          case TokenType::LogicalAndStrong:
            return lhs && rhs;
          case TokenType::LogicalOr:
            return lhs || rhs;
          case TokenType::Comma:
            return rhs;
          default:
            SetInternalError(error, ErrorCode::Syntax, offset);
            return 0;
        }
      }
      case NodeType::Assignment:
      {
        const ResolvedLValue target = left->ResolveLValue(memory_access, error);
        if (error && error->valid)
          return 0;
        const u64 rhs = right->Evaluate(memory_access, error);
        if (error && error->valid)
          return 0;
        u64 value = rhs;
        switch (operation)
        {
          case TokenType::Assign:
            break;
          case TokenType::AssignMultiply:
            value = target.Read(error) * rhs;
            break;
          case TokenType::AssignDivide:
            if (rhs == 0)
            {
              SetInternalError(error, ErrorCode::DivideByZero, right->offset);
              return 0;
            }
            value = target.Read(error) / rhs;
            break;
          case TokenType::AssignModulo:
            if (rhs == 0)
            {
              SetInternalError(error, ErrorCode::DivideByZero, right->offset);
              return 0;
            }
            value = target.Read(error) % rhs;
            break;
          case TokenType::AssignAdd:
            value = target.Read(error) + rhs;
            break;
          case TokenType::AssignSubtract:
            value = target.Read(error) - rhs;
            break;
          case TokenType::AssignShiftLeft:
            value = (rhs < 64) ? (target.Read(error) << rhs) : 0;
            break;
          case TokenType::AssignShiftRight:
            value = (rhs < 64) ? (target.Read(error) >> rhs) : 0;
            break;
          case TokenType::AssignAnd:
            value = target.Read(error) & rhs;
            break;
          case TokenType::AssignXor:
            value = target.Read(error) ^ rhs;
            break;
          case TokenType::AssignOr:
            value = target.Read(error) | rhs;
            break;
          default:
            SetInternalError(error, ErrorCode::Syntax, offset);
            return 0;
        }
        if (error && error->valid)
          return 0;
        target.Write(value, error);
        return (error && error->valid) ? 0 : value;
      }
      default:
        SetInternalError(error, ErrorCode::Syntax, offset);
        return 0;
    }
  }
};

// Precedence follows the MAME debugger expression grammar, including word aliases such as and/band/or.
int GetBinaryPrecedence(TokenType type)
{
  switch (type)
  {
    case TokenType::Multiply:
    case TokenType::Divide:
    case TokenType::Modulo:
      return 13;
    case TokenType::Add:
    case TokenType::Subtract:
      return 12;
    case TokenType::ShiftLeft:
    case TokenType::ShiftRight:
      return 11;
    case TokenType::Less:
    case TokenType::LessEqual:
    case TokenType::Greater:
    case TokenType::GreaterEqual:
      return 10;
    case TokenType::Equal:
    case TokenType::NotEqual:
      return 9;
    case TokenType::BitwiseAnd:
    case TokenType::LogicalAndStrong:
      return 8;
    case TokenType::BitwiseXor:
      return 7;
    case TokenType::BitwiseOr:
      return 6;
    case TokenType::LogicalAnd:
      return 5;
    case TokenType::LogicalOr:
      return 4;
    case TokenType::Assign:
    case TokenType::AssignMultiply:
    case TokenType::AssignDivide:
    case TokenType::AssignModulo:
    case TokenType::AssignAdd:
    case TokenType::AssignSubtract:
    case TokenType::AssignShiftLeft:
    case TokenType::AssignShiftRight:
    case TokenType::AssignAnd:
    case TokenType::AssignXor:
    case TokenType::AssignOr:
      return 3;
    case TokenType::Comma:
      return 2;
    default:
      return -1;
  }
}

bool IsAssignment(TokenType type)
{
  return type >= TokenType::Assign && type <= TokenType::AssignOr;
}

class Parser
{
public:
  Parser(std::string_view expression, const std::unordered_map<std::string, Symbol>& symbols,
         const ExpressionMemoryAccess* memory)
    : m_lexer(expression, symbols, memory, &m_error)
  {
    Advance();
  }

  std::unique_ptr<Node> Parse()
  {
    if (m_error.valid)
      return {};
    if (m_current.type == TokenType::End)
      return {};

    std::unique_ptr<Node> result = ParseExpression(0);
    if (m_error.valid)
      return {};
    if (m_current.type != TokenType::End)
    {
      SetInternalError(&m_error, ErrorCode::Syntax, m_current.offset);
      return {};
    }
    return result;
  }

  const ExpressionErrorInfo& GetError() const { return m_error; }

private:
  void Advance()
  {
    if (!m_error.valid)
      m_current = m_lexer.Next();
  }

  std::unique_ptr<Node> ParseExpression(int minimum_precedence)
  {
    std::unique_ptr<Node> left = ParsePrefix();
    if (m_error.valid)
      return {};

    for (;;)
    {
      if (m_current.type == TokenType::PrefixIncrement || m_current.type == TokenType::PrefixDecrement)
      {
        if (15 < minimum_precedence)
          break;
        const Token token = m_current;
        Advance();
        if (m_error.valid)
          return {};

        auto node = std::make_unique<Node>();
        node->type = (token.type == TokenType::PrefixIncrement) ? NodeType::PostfixIncrement : NodeType::PostfixDecrement;
        node->offset = token.offset;
        node->left = std::move(left);
        left = std::move(node);
        continue;
      }

      const int precedence = GetBinaryPrecedence(m_current.type);
      if (precedence < minimum_precedence)
        break;

      const Token operation = m_current;
      Advance();
      if (m_error.valid)
        return {};

      const bool right_associative = IsAssignment(operation.type);
      std::unique_ptr<Node> right = ParseExpression(right_associative ? precedence : precedence + 1);
      if (m_error.valid)
        return {};

      auto node = std::make_unique<Node>();
      node->type = right_associative ? NodeType::Assignment : NodeType::Binary;
      node->operation = operation.type;
      node->offset = operation.offset;
      node->left = std::move(left);
      node->right = std::move(right);
      left = std::move(node);
    }

    return left;
  }

  std::unique_ptr<Node> ParsePrefix()
  {
    const Token token = m_current;
    switch (token.type)
    {
      case TokenType::PrefixIncrement:
      case TokenType::PrefixDecrement:
      {
        Advance();
        if (m_error.valid)
          return {};

        auto node = std::make_unique<Node>();
        node->type = (token.type == TokenType::PrefixIncrement) ? NodeType::PrefixIncrement : NodeType::PrefixDecrement;
        node->offset = token.offset;
        node->left = ParseExpression(14);
        if (m_error.valid)
          return {};
        return node;
      }
      case TokenType::LogicalNot:
      case TokenType::BitwiseNot:
      {
        Advance();
        if (m_error.valid)
          return {};

        auto node = std::make_unique<Node>();
        node->type = NodeType::Unary;
        node->operation = token.type;
        node->offset = token.offset;
        node->left = ParseExpression(14);
        if (m_error.valid)
          return {};
        return node;
      }
      case TokenType::Add:
      case TokenType::Subtract:
      {
        Advance();
        if (m_error.valid)
          return {};

        auto node = std::make_unique<Node>();
        node->type = NodeType::Unary;
        node->operation = (token.type == TokenType::Add) ? TokenType::UnaryPlus : TokenType::UnaryMinus;
        node->offset = token.offset;
        node->left = ParseExpression(14);
        if (m_error.valid)
          return {};
        return node;
      }
      case TokenType::Memory:
      {
        Advance();
        if (m_error.valid)
          return {};

        auto node = std::make_unique<Node>();
        node->type = NodeType::Memory;
        node->offset = token.offset;
        node->memory = token.memory;
        node->left = ParseExpression(14);
        if (m_error.valid)
          return {};
        return node;
      }
      default:
        return ParsePrimary();
    }
  }

  std::unique_ptr<Node> ParsePrimary()
  {
    const Token token = m_current;
    switch (token.type)
    {
      case TokenType::Number:
      {
        Advance();
        if (m_error.valid)
          return {};

        auto node = std::make_unique<Node>();
        node->type = NodeType::Number;
        node->offset = token.offset;
        node->number = token.number;
        return node;
      }
      case TokenType::String:
      {
        Advance();
        if (m_error.valid)
          return {};

        auto node = std::make_unique<Node>();
        node->type = NodeType::String;
        node->offset = token.offset;
        node->text = token.text;
        return node;
      }
      case TokenType::Symbol:
      {
        Advance();
        if (m_error.valid)
          return {};

        auto node = std::make_unique<Node>();
        node->type = NodeType::Symbol;
        node->offset = token.offset;
        node->symbol = token.symbol;
        return node;
      }
      case TokenType::Function:
        return ParseFunction();
      case TokenType::LeftParen:
      {
        Advance();
        if (m_error.valid)
          return {};

        std::unique_ptr<Node> node = ParseExpression(0);
        if (m_error.valid)
          return {};
        if (m_current.type != TokenType::RightParen)
        {
          SetInternalError(&m_error, ErrorCode::UnbalancedParentheses, token.offset);
          return {};
        }
        Advance();
        if (m_error.valid)
          return {};
        return node;
      }
      default:
        SetInternalError(&m_error, ErrorCode::Syntax, token.offset);
        return {};
    }
  }

  std::unique_ptr<Node> ParseFunction()
  {
    const Token function = m_current;
    Advance();
    if (m_error.valid)
      return {};
    if (m_current.type != TokenType::LeftParen)
    {
      SetInternalError(&m_error, ErrorCode::Syntax, function.offset);
      return {};
    }
    Advance();
    if (m_error.valid)
      return {};

    auto node = std::make_unique<Node>();
    node->type = NodeType::Function;
    node->offset = function.offset;
    node->symbol = function.symbol;

    if (m_current.type != TokenType::RightParen)
    {
      for (;;)
      {
        node->arguments.push_back(ParseExpression(3));
        if (m_error.valid)
          return {};
        if (m_current.type != TokenType::Comma)
          break;
        Advance();
        if (m_error.valid)
          return {};
      }
    }

    if (m_current.type != TokenType::RightParen)
    {
      SetInternalError(&m_error, ErrorCode::UnbalancedParentheses, function.offset);
      return {};
    }
    Advance();
    if (m_error.valid)
      return {};
    return node;
  }

  ExpressionErrorInfo m_error;
  Lexer m_lexer;
  Token m_current;
};

} // namespace

struct ExpressionSymbolTable::Impl
{
  std::unordered_map<std::string, Symbol> symbols;
};

ExpressionSymbolTable::ExpressionSymbolTable() : m_impl(std::make_unique<Impl>())
{
}

ExpressionSymbolTable::~ExpressionSymbolTable() = default;

void ExpressionSymbolTable::AddValue(std::string_view name, u64* value)
{
  AddValue(name, [value]() { return *value; }, [value](u64 new_value) { *value = new_value; });
}

void ExpressionSymbolTable::AddValue(std::string_view name, Getter getter, Setter setter)
{
  Symbol symbol;
  symbol.type = SymbolType::Value;
  symbol.getter = std::move(getter);
  symbol.setter = std::move(setter);
  m_impl->symbols.insert_or_assign(Lowercase(name), std::move(symbol));
}

void ExpressionSymbolTable::AddConstant(std::string_view name, u64 value)
{
  Symbol symbol;
  symbol.type = SymbolType::Value;
  symbol.getter = [value]() { return value; };
  m_impl->symbols.insert_or_assign(Lowercase(name), std::move(symbol));
}

void ExpressionSymbolTable::AddFunction(std::string_view name, u32 minimum_parameters, u32 maximum_parameters,
                                        Function function)
{
  Symbol symbol;
  symbol.type = SymbolType::Function;
  symbol.function = std::move(function);
  symbol.minimum_parameters = minimum_parameters;
  symbol.maximum_parameters = maximum_parameters;
  m_impl->symbols.insert_or_assign(Lowercase(name), std::move(symbol));
}

void ExpressionSymbolTable::Clear()
{
  m_impl->symbols.clear();
}

struct Expression::Impl
{
  Impl(ExpressionSymbolTable* symbols_, const ExpressionMemoryAccess* memory_) : symbols(symbols_), memory(memory_) {}

  ExpressionSymbolTable* symbols;
  const ExpressionMemoryAccess* memory;
  std::string original_string;
  std::unique_ptr<Node> root;
};

Expression::Expression(ExpressionSymbolTable* symbols, const ExpressionMemoryAccess* memory)
  : m_impl(std::make_unique<Impl>(symbols, memory))
{
}

Expression::~Expression() = default;
Expression::Expression(Expression&&) noexcept = default;
Expression& Expression::operator=(Expression&&) noexcept = default;

bool Expression::Parse(std::string_view expression, Error* error)
{
  m_impl->original_string.assign(expression);
  m_impl->root.reset();
  if (!m_impl->symbols)
  {
    Error::SetStringView(error, "Expression has no symbol table");
    return false;
  }

  Parser parser(expression, m_impl->symbols->m_impl->symbols, m_impl->memory);
  std::unique_ptr<Node> root = parser.Parse();
  if (parser.GetError().valid)
  {
    SetExpressionError(error, parser.GetError());
    return false;
  }

  m_impl->root = std::move(root);
  return true;
}

bool Expression::Execute(u64* result, Error* error) const
{
  if (!m_impl->root)
  {
    if (result)
      *result = 0;
    return true;
  }

  ExpressionErrorInfo expression_error;
  const u64 value = m_impl->root->Evaluate(m_impl->memory, &expression_error);
  if (expression_error.valid)
  {
    SetExpressionError(error, expression_error);
    return false;
  }

  if (result)
    *result = value;
  return true;
}

bool Expression::IsEmpty() const
{
  return !m_impl->root;
}

const std::string& Expression::GetOriginalString() const
{
  return m_impl->original_string;
}

u64 FromBCD(u64 value)
{
  u64 multiplier = 1;
  u64 result = 0;
  while (value != 0)
  {
    result += (value & 0x0F) * multiplier;
    value >>= 4;
    multiplier *= 10;
  }
  return result;
}

u64 ToBCD(u64 value)
{
  u64 result = 0;
  u32 shift = 0;
  while (value != 0 && shift < 64)
  {
    result += (value % 10) << shift;
    value /= 10;
    shift += 4;
  }
  return result;
}

} // namespace MAMECheats
