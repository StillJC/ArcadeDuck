// SPDX-FileCopyrightText: 2026 StillJC
// SPDX-License-Identifier: GPL-3.0-only

#include "mame_cheats.h"

#include "common/error.h"
#include "common/file_system.h"
#include "common/log.h"

#include <algorithm>
#include <charconv>
#include <cctype>
#include <limits>
#include <utility>

Log_SetChannel(MAMECheats);

namespace MAMECheats {
namespace {

struct XMLAttribute
{
  std::string name;
  std::string value;
};

struct XMLNode
{
  std::string name;
  std::string value;
  std::vector<XMLAttribute> attributes;
  std::vector<XMLNode> children;
  u32 line = 1;

  const std::string* GetAttribute(std::string_view attribute_name) const
  {
    const auto it = std::find_if(attributes.begin(), attributes.end(), [attribute_name](const XMLAttribute& attribute) {
      return attribute.name == attribute_name;
    });
    return (it != attributes.end()) ? &it->value : nullptr;
  }
};

bool IsNameStart(char ch)
{
  const unsigned char uch = static_cast<unsigned char>(ch);
  return std::isalpha(uch) != 0 || ch == '_' || ch == ':';
}

bool IsNameCharacter(char ch)
{
  const unsigned char uch = static_cast<unsigned char>(ch);
  return IsNameStart(ch) || std::isdigit(uch) != 0 || ch == '-' || ch == '.';
}

bool IsWhitespace(char ch)
{
  return ch == ' ' || ch == '\t' || ch == '\r' || ch == '\n';
}

std::string_view TrimWhitespace(std::string_view value)
{
  while (!value.empty() && IsWhitespace(value.front()))
    value.remove_prefix(1);
  while (!value.empty() && IsWhitespace(value.back()))
    value.remove_suffix(1);
  return value;
}

bool AppendCodepointUTF8(std::string* destination, u32 codepoint)
{
  if (codepoint > 0x10FFFF || (codepoint >= 0xD800 && codepoint <= 0xDFFF))
    return false;

  if (codepoint <= 0x7F)
  {
    destination->push_back(static_cast<char>(codepoint));
  }
  else if (codepoint <= 0x7FF)
  {
    destination->push_back(static_cast<char>(0xC0 | (codepoint >> 6)));
    destination->push_back(static_cast<char>(0x80 | (codepoint & 0x3F)));
  }
  else if (codepoint <= 0xFFFF)
  {
    destination->push_back(static_cast<char>(0xE0 | (codepoint >> 12)));
    destination->push_back(static_cast<char>(0x80 | ((codepoint >> 6) & 0x3F)));
    destination->push_back(static_cast<char>(0x80 | (codepoint & 0x3F)));
  }
  else
  {
    destination->push_back(static_cast<char>(0xF0 | (codepoint >> 18)));
    destination->push_back(static_cast<char>(0x80 | ((codepoint >> 12) & 0x3F)));
    destination->push_back(static_cast<char>(0x80 | ((codepoint >> 6) & 0x3F)));
    destination->push_back(static_cast<char>(0x80 | (codepoint & 0x3F)));
  }
  return true;
}

// Cheat XML is user-supplied and small enough to parse as a self-contained tree. Expressions and output
// strings are preserved verbatim; only XML syntax and the MAME cheat schema are interpreted here.
class XMLParser
{
public:
  XMLParser(std::string_view filename, std::string_view input, Error* error)
    : m_filename(filename), m_input(input), m_error(error)
  {
  }

  bool Parse(XMLNode* root)
  {
    if (m_input.size() >= 3 && static_cast<u8>(m_input[0]) == 0xEF && static_cast<u8>(m_input[1]) == 0xBB &&
        static_cast<u8>(m_input[2]) == 0xBF)
    {
      m_position = 3;
    }

    if (!SkipDocumentTrivia())
      return false;
    if (AtEnd())
      return Fail("missing root element");
    if (!ParseElement(root))
      return false;
    if (!SkipDocumentTrivia())
      return false;
    if (!AtEnd())
      return Fail("unexpected content after root element");
    return true;
  }

private:
  bool AtEnd() const { return m_position >= m_input.size(); }

  bool StartsWith(std::string_view text) const
  {
    return m_position <= m_input.size() && m_input.substr(m_position).starts_with(text);
  }

  void Advance(size_t count)
  {
    const size_t end = std::min(m_position + count, m_input.size());
    for (; m_position < end; m_position++)
    {
      if (m_input[m_position] == '\n')
        m_line++;
    }
  }

  bool Fail(std::string_view message)
  {
    Error::SetStringFmt(m_error, "{}({}): {}", m_filename, m_line, message);
    return false;
  }

  bool FailAt(u32 line, std::string_view message)
  {
    Error::SetStringFmt(m_error, "{}({}): {}", m_filename, line, message);
    return false;
  }

  void SkipWhitespace()
  {
    while (!AtEnd() && IsWhitespace(m_input[m_position]))
      Advance(1);
  }

  bool SkipDocumentTrivia()
  {
    for (;;)
    {
      SkipWhitespace();
      if (StartsWith("<!--"))
      {
        if (!SkipComment())
          return false;
      }
      else if (StartsWith("<?"))
      {
        if (!SkipProcessingInstruction())
          return false;
      }
      else
      {
        return true;
      }
    }
  }

  bool SkipComment()
  {
    const size_t end = m_input.find("-->", m_position + 4);
    if (end == std::string_view::npos)
      return Fail("unterminated XML comment");
    Advance((end + 3) - m_position);
    return true;
  }

  bool SkipProcessingInstruction()
  {
    const size_t end = m_input.find("?>", m_position + 2);
    if (end == std::string_view::npos)
      return Fail("unterminated XML processing instruction");
    Advance((end + 2) - m_position);
    return true;
  }

  bool ParseName(std::string* name)
  {
    if (AtEnd() || !IsNameStart(m_input[m_position]))
      return Fail("expected XML name");

    const size_t start = m_position;
    Advance(1);
    while (!AtEnd() && IsNameCharacter(m_input[m_position]))
      Advance(1);
    name->assign(m_input.substr(start, m_position - start));
    return true;
  }

  bool ParseEntity(std::string* destination)
  {
    const u32 entity_line = m_line;
    const size_t semicolon = m_input.find(';', m_position + 1);
    if (semicolon == std::string_view::npos)
      return FailAt(entity_line, "unterminated XML entity");

    const std::string_view entity = m_input.substr(m_position + 1, semicolon - m_position - 1);
    if (entity == "amp")
      destination->push_back('&');
    else if (entity == "lt")
      destination->push_back('<');
    else if (entity == "gt")
      destination->push_back('>');
    else if (entity == "quot")
      destination->push_back('"');
    else if (entity == "apos")
      destination->push_back('\'');
    else if (entity.starts_with("#"))
    {
      std::string_view number = entity.substr(1);
      int base = 10;
      if (number.starts_with("x") || number.starts_with("X"))
      {
        number.remove_prefix(1);
        base = 16;
      }

      u32 codepoint = 0;
      const auto result = std::from_chars(number.data(), number.data() + number.size(), codepoint, base);
      if (number.empty() || result.ec != std::errc() || result.ptr != number.data() + number.size() ||
          !AppendCodepointUTF8(destination, codepoint))
      {
        return FailAt(entity_line, "invalid numeric XML entity");
      }
    }
    else
    {
      return FailAt(entity_line, "unsupported XML entity");
    }

    Advance((semicolon + 1) - m_position);
    return true;
  }

  bool ParseText(std::string* destination, char terminator = '<')
  {
    while (!AtEnd() && m_input[m_position] != terminator)
    {
      if (m_input[m_position] == '&')
      {
        if (!ParseEntity(destination))
          return false;
      }
      else
      {
        destination->push_back(m_input[m_position]);
        Advance(1);
      }
    }
    return true;
  }

  bool ParseAttribute(XMLAttribute* attribute)
  {
    if (!ParseName(&attribute->name))
      return false;
    SkipWhitespace();
    if (AtEnd() || m_input[m_position] != '=')
      return Fail("expected '=' after XML attribute name");
    Advance(1);
    SkipWhitespace();
    if (AtEnd() || (m_input[m_position] != '"' && m_input[m_position] != '\''))
      return Fail("expected quoted XML attribute value");

    const char quote = m_input[m_position];
    Advance(1);
    if (!ParseText(&attribute->value, quote))
      return false;
    if (AtEnd())
      return Fail("unterminated XML attribute value");
    Advance(1);
    return true;
  }

  bool ParseCDATA(std::string* destination)
  {
    const size_t start = m_position + 9;
    const size_t end = m_input.find("]]>", start);
    if (end == std::string_view::npos)
      return Fail("unterminated CDATA section");
    Advance(9);
    destination->append(m_input.substr(m_position, end - m_position));
    Advance((end + 3) - m_position);
    return true;
  }

  bool ParseElement(XMLNode* node)
  {
    if (AtEnd() || m_input[m_position] != '<' || StartsWith("</") || StartsWith("<!") || StartsWith("<?"))
      return Fail("expected XML element");

    node->line = m_line;
    Advance(1);
    if (!ParseName(&node->name))
      return false;

    for (;;)
    {
      SkipWhitespace();
      if (AtEnd())
        return FailAt(node->line, "unterminated XML element");
      if (StartsWith("/>"))
      {
        Advance(2);
        return true;
      }
      if (m_input[m_position] == '>')
      {
        Advance(1);
        break;
      }

      XMLAttribute attribute;
      if (!ParseAttribute(&attribute))
        return false;
      const bool duplicate =
        std::any_of(node->attributes.begin(), node->attributes.end(),
                    [&attribute](const XMLAttribute& current) { return current.name == attribute.name; });
      if (duplicate)
        return Fail("duplicate XML attribute");
      node->attributes.push_back(std::move(attribute));
    }

    for (;;)
    {
      if (AtEnd())
        return FailAt(node->line, "unterminated XML element");
      if (StartsWith("</"))
      {
        Advance(2);
        std::string closing_name;
        if (!ParseName(&closing_name))
          return false;
        SkipWhitespace();
        if (AtEnd() || m_input[m_position] != '>')
          return Fail("expected '>' after XML closing element");
        Advance(1);
        if (closing_name != node->name)
          return FailAt(node->line, "mismatched XML closing element");
        return true;
      }
      if (StartsWith("<!--"))
      {
        if (!SkipComment())
          return false;
      }
      else if (StartsWith("<![CDATA["))
      {
        if (!ParseCDATA(&node->value))
          return false;
      }
      else if (StartsWith("<?"))
      {
        if (!SkipProcessingInstruction())
          return false;
      }
      else if (m_input[m_position] == '<')
      {
        XMLNode child;
        if (!ParseElement(&child))
          return false;
        node->children.push_back(std::move(child));
      }
      else if (!ParseText(&node->value))
      {
        return false;
      }
    }
  }

  std::string_view m_filename;
  std::string_view m_input;
  Error* m_error;
  size_t m_position = 0;
  u32 m_line = 1;
};

bool ParseUnsigned(std::string_view text, u64* value, NumberFormat* format = nullptr)
{
  text = TrimWhitespace(text);
  if (text.empty())
    return false;

  int base = 10;
  NumberFormat parsed_format = NumberFormat::Decimal;
  if (text.front() == '#')
  {
    text.remove_prefix(1);
    parsed_format = NumberFormat::DecimalHash;
  }
  else if (text.front() == '$')
  {
    text.remove_prefix(1);
    base = 16;
    parsed_format = NumberFormat::HexDollar;
  }
  else if (text.size() > 2 && text[0] == '0' && (text[1] == 'x' || text[1] == 'X'))
  {
    text.remove_prefix(2);
    base = 16;
    parsed_format = NumberFormat::HexC;
  }

  if (text.empty())
    return false;

  u64 parsed = 0;
  const auto result = std::from_chars(text.data(), text.data() + text.size(), parsed, base);
  if (result.ec != std::errc() || result.ptr != text.data() + text.size())
    return false;

  *value = parsed;
  if (format)
    *format = parsed_format;
  return true;
}

bool ParseNumberAttribute(const XMLNode& node, std::string_view name, Number default_value, Number* value, Error* error,
                          std::string_view filename)
{
  const std::string* attribute = node.GetAttribute(name);
  if (!attribute)
  {
    *value = default_value;
    return true;
  }

  Number parsed;
  if (!ParseUnsigned(*attribute, &parsed.value, &parsed.format))
  {
    Error::SetStringFmt(error, "{}({}): invalid '{}' value '{}'", filename, node.line, name, *attribute);
    return false;
  }
  *value = parsed;
  return true;
}

bool ParseU32Attribute(const XMLNode& node, std::string_view name, u32 default_value, u32* value, Error* error,
                       std::string_view filename)
{
  const std::string* attribute = node.GetAttribute(name);
  if (!attribute)
  {
    *value = default_value;
    return true;
  }

  u64 parsed = 0;
  if (!ParseUnsigned(*attribute, &parsed) || parsed > std::numeric_limits<u32>::max())
  {
    Error::SetStringFmt(error, "{}({}): invalid '{}' value '{}'", filename, node.line, name, *attribute);
    return false;
  }
  *value = static_cast<u32>(parsed);
  return true;
}

bool ParseS32Attribute(const XMLNode& node, std::string_view name, s32 default_value, s32* value, Error* error,
                       std::string_view filename)
{
  const std::string* attribute = node.GetAttribute(name);
  if (!attribute)
  {
    *value = default_value;
    return true;
  }

  const std::string_view text = TrimWhitespace(*attribute);
  s32 parsed = 0;
  const auto result = std::from_chars(text.data(), text.data() + text.size(), parsed, 10);
  if (text.empty() || result.ec != std::errc() || result.ptr != text.data() + text.size())
  {
    Error::SetStringFmt(error, "{}({}): invalid '{}' value '{}'", filename, node.line, name, *attribute);
    return false;
  }
  *value = parsed;
  return true;
}

std::string TrimmedString(std::string_view value)
{
  value = TrimWhitespace(value);
  return std::string(value);
}

bool ParseParameter(const XMLNode& node, Parameter* parameter, Error* error, std::string_view filename)
{
  if (!ParseNumberAttribute(node, "min", {}, &parameter->minimum, error, filename) ||
      !ParseNumberAttribute(node, "max", {}, &parameter->maximum, error, filename) ||
      !ParseNumberAttribute(node, "step", {1, NumberFormat::Decimal}, &parameter->step, error, filename))
  {
    return false;
  }

  for (const XMLNode& child : node.children)
  {
    if (child.name != "item")
      continue;

    const std::string* value_attribute = child.GetAttribute("value");
    if (!value_attribute)
    {
      Error::SetStringFmt(error, "{}({}): item is missing value", filename, child.line);
      return false;
    }

    ParameterItem item;
    item.text = TrimmedString(child.value);
    if (item.text.empty())
    {
      Error::SetStringFmt(error, "{}({}): item is missing text", filename, child.line);
      return false;
    }
    if (!ParseUnsigned(*value_attribute, &item.value.value, &item.value.format))
    {
      Error::SetStringFmt(error, "{}({}): invalid item value '{}'", filename, child.line, *value_attribute);
      return false;
    }

    if (item.value.value > parameter->maximum.value)
    {
      parameter->maximum.value = item.value.value;
      parameter->maximum.format = item.value.format;
    }
    parameter->items.push_back(std::move(item));
  }
  return true;
}

// MAME cheat output accepts an integer-only printf subset and expands argument count= before formatting.
bool ValidateOutputFormat(const ScriptEntry& entry, Error* error, std::string_view filename, u32 line)
{
  u32 provided = 0;
  for (const OutputArgument& argument : entry.arguments)
  {
    if (argument.count > (MAX_OUTPUT_ARGUMENTS - provided))
    {
      Error::SetStringFmt(error, "{}({}): too many output arguments (max {})", filename, line, MAX_OUTPUT_ARGUMENTS);
      return false;
    }
    provided += argument.count;
  }

  u32 expected = 0;
  for (size_t position = 0; (position = entry.format.find('%', position)) != std::string::npos;)
  {
    position++;
    while (position < entry.format.size() &&
           std::string_view("lh0123456789.-+ #").find(entry.format[position]) != std::string_view::npos)
    {
      position++;
    }
    if (position >= entry.format.size() ||
        std::string_view("cdiouxX").find(entry.format[position]) == std::string_view::npos)
    {
      Error::SetStringFmt(error, "{}({}): invalid output format '{}'", filename, line, entry.format);
      return false;
    }
    expected++;
    position++;
  }

  if (provided != expected)
  {
    Error::SetStringFmt(error, "{}({}): output format expects {} arguments but {} provided", filename, line, expected,
                        provided);
    return false;
  }
  return true;
}

bool ParseScriptEntry(const XMLNode& node, ScriptEntry* entry, Error* error, std::string_view filename)
{
  if (const std::string* condition = node.GetAttribute("condition"))
    entry->condition = *condition;

  if (node.name == "action")
  {
    entry->type = ScriptEntryType::Action;
    entry->expression = TrimmedString(node.value);
    if (entry->expression.empty())
    {
      Error::SetStringFmt(error, "{}({}): missing expression in action tag", filename, node.line);
      return false;
    }
    return true;
  }

  entry->type = ScriptEntryType::Output;
  const std::string* format = node.GetAttribute("format");
  if (!format || format->empty())
  {
    Error::SetStringFmt(error, "{}({}): missing format in output tag", filename, node.line);
    return false;
  }
  entry->format = *format;

  if (!ParseS32Attribute(node, "line", 0, &entry->line, error, filename))
    return false;

  if (const std::string* align = node.GetAttribute("align"))
  {
    if (*align == "center")
      entry->alignment = OutputAlignment::Center;
    else if (*align == "right")
      entry->alignment = OutputAlignment::Right;
    else if (*align != "left")
    {
      Error::SetStringFmt(error, "{}({}): invalid output alignment '{}'", filename, node.line, *align);
      return false;
    }
  }

  for (const XMLNode& child : node.children)
  {
    if (child.name != "argument")
      continue;

    OutputArgument argument;
    argument.expression = TrimmedString(child.value);
    if (argument.expression.empty())
    {
      Error::SetStringFmt(error, "{}({}): missing expression in argument tag", filename, child.line);
      return false;
    }
    if (!ParseU32Attribute(child, "count", 1, &argument.count, error, filename))
      return false;
    entry->arguments.push_back(std::move(argument));
  }

  return ValidateOutputFormat(*entry, error, filename, node.line);
}

bool ParseScript(const XMLNode& node, Script* script, Error* error, std::string_view filename)
{
  const std::string* state = node.GetAttribute("state");
  if (!state || *state == "run")
    script->state = ScriptState::Run;
  else if (*state == "on")
    script->state = ScriptState::On;
  else if (*state == "off")
    script->state = ScriptState::Off;
  else if (*state == "change")
    script->state = ScriptState::Change;
  else
  {
    Error::SetStringFmt(error, "{}({}): invalid script state '{}'", filename, node.line, *state);
    return false;
  }

  for (const XMLNode& child : node.children)
  {
    if (child.name != "action" && child.name != "output")
    {
      WARNING_LOG("{}({}): unknown script item '{}' ignored", filename, child.line, child.name);
      continue;
    }

    ScriptEntry entry;
    if (!ParseScriptEntry(child, &entry, error, filename))
      return false;
    script->entries.push_back(std::move(entry));
  }
  return true;
}

bool ParseCheat(const XMLNode& node, Entry* entry, Error* error, std::string_view filename)
{
  const std::string* description = node.GetAttribute("desc");
  if (!description || description->empty())
  {
    Error::SetStringFmt(error, "{}({}): empty or missing desc attribute on cheat", filename, node.line);
    return false;
  }
  entry->description = *description;

  if (!ParseU32Attribute(node, "tempvariables", DEFAULT_TEMP_VARIABLES, &entry->temp_variables, error, filename) ||
      entry->temp_variables == 0)
  {
    if (entry->temp_variables == 0)
      Error::SetStringFmt(error, "{}({}): tempvariables must be greater than zero", filename, node.line);
    return false;
  }

  bool have_comment = false;
  bool have_parameter = false;
  for (const XMLNode& child : node.children)
  {
    if (child.name == "comment")
    {
      if (have_comment)
      {
        WARNING_LOG("{}({}): additional comment node ignored", filename, child.line);
        continue;
      }
      entry->comment = TrimmedString(child.value);
      have_comment = true;
    }
    else if (child.name == "parameter")
    {
      if (have_parameter)
      {
        WARNING_LOG("{}({}): additional parameter node ignored", filename, child.line);
        continue;
      }
      Parameter parameter;
      if (!ParseParameter(child, &parameter, error, filename))
        return false;
      entry->parameter = std::move(parameter);
      have_parameter = true;
    }
    else if (child.name == "script")
    {
      Script script;
      if (!ParseScript(child, &script, error, filename))
        return false;
      std::optional<Script>& slot = entry->scripts[static_cast<size_t>(script.state)];
      if (slot.has_value())
      {
        WARNING_LOG("{}({}): additional '{}' script ignored", filename, child.line,
                    child.GetAttribute("state") ? *child.GetAttribute("state") : "run");
        continue;
      }
      slot = std::move(script);
    }
  }
  return true;
}

} // namespace

// These predicates mirror MAME's menu classification; script presence determines how an entry is controlled.
bool Entry::IsTextOnly() const
{
  return !parameter.has_value() && !HasScript(ScriptState::Run) && !HasScript(ScriptState::Off) &&
         !HasScript(ScriptState::On);
}

bool Entry::IsOneShot() const
{
  return !parameter.has_value() && !HasScript(ScriptState::Run) && !HasScript(ScriptState::Off) &&
         HasScript(ScriptState::On);
}

bool Entry::IsOnOff() const
{
  return !parameter.has_value() &&
         (HasScript(ScriptState::Run) || (HasScript(ScriptState::Off) && HasScript(ScriptState::On)));
}

bool Entry::IsValueParameter() const
{
  return parameter.has_value() && !parameter->HasItemList();
}

bool Entry::IsItemListParameter() const
{
  return parameter.has_value() && parameter->HasItemList();
}

bool Entry::IsOneShotParameter() const
{
  return parameter.has_value() && !HasScript(ScriptState::Run) && !HasScript(ScriptState::Off) &&
         HasScript(ScriptState::Change);
}

bool ParseXML(std::string_view filename, std::string_view xml, List* list, Error* error)
{
  if (!list)
  {
    Error::SetString(error, "MAME cheat output list is null.");
    return false;
  }

  XMLNode root;
  XMLParser parser(filename, xml, error);
  if (!parser.Parse(&root))
    return false;
  if (root.name != "mamecheat")
  {
    Error::SetStringFmt(error, "{}({}): expected mamecheat root element", filename, root.line);
    return false;
  }

  const std::string* version = root.GetAttribute("version");
  u64 parsed_version = 0;
  if (!version || !ParseUnsigned(*version, &parsed_version) || parsed_version != 1)
  {
    Error::SetStringFmt(error, "{}({}): unsupported or missing MAME cheat version", filename, root.line);
    return false;
  }

  List parsed_list;
  for (const XMLNode& child : root.children)
  {
    if (child.name != "cheat")
    {
      WARNING_LOG("{}({}): unknown mamecheat item '{}' ignored", filename, child.line, child.name);
      continue;
    }

    Entry entry;
    if (!ParseCheat(child, &entry, error, filename))
      return false;
    parsed_list.GetEntries().push_back(std::move(entry));
  }

  *list = std::move(parsed_list);
  return true;
}

bool LoadXMLFile(const char* filename, List* list, Error* error)
{
  std::optional<std::string> xml = FileSystem::ReadFileToString(filename, error);
  if (!xml.has_value())
    return false;
  return ParseXML(filename, *xml, list, error);
}

} // namespace MAMECheats
