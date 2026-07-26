// HitParser.cxx
#include "HitParser.h"

#include <QFile>
#include <QTextStream>

#include <cctype>
#include <fstream>
#include <sstream>

std::string HitNode::path() const
{
  if (!parent || parent->name.empty())
  {
    return name;
  }
  std::string parentPath = parent->path();
  return parentPath.empty() ? name : parentPath + "/" + name;
}

// ---------------------------------------------------------------------------
// Tokenizer
// ---------------------------------------------------------------------------
std::vector<HitParser::Token> HitParser::tokenize(const std::string& text)
{
  std::vector<Token> tokens;
  int line = 1;
  size_t i = 0;
  const size_t n = text.size();

  auto isIdentChar = [](char c) {
    return !std::isspace(static_cast<unsigned char>(c)) && c != '[' && c != ']' && c != '=' && c != '#' &&
      c != '\'' && c != '"';
  };

  while (i < n)
  {
    char c = text[i];

    if (c == '\n')
    {
      ++line;
      ++i;
      continue;
    }
    if (std::isspace(static_cast<unsigned char>(c)))
    {
      ++i;
      continue;
    }
    if (c == '#')
    {
      // Comment to end of line.
      while (i < n && text[i] != '\n')
      {
        ++i;
      }
      continue;
    }
    if (c == '=')
    {
      // '=' is a plain separator, not emitted as its own token; the parser
      // consumes it positionally between identifier and value.
      ++i;
      continue;
    }
    if (c == '[')
    {
      size_t start = i;
      ++i;
      // "[]" is a close marker.
      if (i < n && text[i] == ']')
      {
        ++i;
        tokens.push_back({ Token::Kind::BlockClose, "[]", line });
        continue;
      }
      std::string name;
      while (i < n && text[i] != ']')
      {
        name += text[i];
        ++i;
      }
      if (i < n)
      {
        ++i; // consume ']'
      }
      tokens.push_back({ Token::Kind::BlockOpen, name, line });
      (void)start;
      continue;
    }
    if (c == '\'' || c == '"')
    {
      char quote = c;
      ++i;
      std::string value;
      while (i < n && text[i] != quote)
      {
        value += text[i];
        ++i;
      }
      if (i < n)
      {
        ++i; // consume closing quote
      }
      tokens.push_back({ Token::Kind::String, value, line });
      continue;
    }

    // Bare identifier / value.
    std::string ident;
    while (i < n && isIdentChar(text[i]))
    {
      ident += text[i];
      ++i;
    }
    if (!ident.empty())
    {
      tokens.push_back({ Token::Kind::Identifier, ident, line });
    }
    else
    {
      // Unrecognized character; skip to avoid an infinite loop.
      ++i;
    }
  }

  return tokens;
}

// ---------------------------------------------------------------------------
// Recursive-descent parse of one block body, i.e. everything between a
// BlockOpen and its matching BlockClose (or, for the implicit root, the
// entire token stream).
// ---------------------------------------------------------------------------
void HitParser::parseBlockBody(
  const std::vector<Token>& tokens, size_t& pos, const std::shared_ptr<HitNode>& node, std::string& errorMessage)
{
  while (pos < tokens.size())
  {
    const Token& tok = tokens[pos];

    if (tok.kind == Token::Kind::BlockClose)
    {
      ++pos; // consume, caller matches it to the open
      return;
    }

    if (tok.kind == Token::Kind::BlockOpen)
    {
      auto child = std::make_shared<HitNode>();
      child->name = tok.text;
      child->parent = node.get();
      ++pos;
      parseBlockBody(tokens, pos, child, errorMessage);
      node->children.push_back(child);
      continue;
    }

    if (tok.kind == Token::Kind::Identifier)
    {
      // Expect `key = value` (value may be Identifier or String).
      std::string key = tok.text;
      ++pos;
      if (pos >= tokens.size() ||
        (tokens[pos].kind != Token::Kind::Identifier && tokens[pos].kind != Token::Kind::String))
      {
        errorMessage += "Line " + std::to_string(tok.line) + ": expected value for parameter '" + key + "'.\n";
        continue;
      }
      std::string value = tokens[pos].text;
      ++pos;
      node->params.push_back({ key, value });
      continue;
    }

    // Stray String token at block scope (shouldn't normally happen).
    ++pos;
  }
  // Reaching end-of-stream without a BlockClose is only valid for the
  // implicit root node; callers can inspect errorMessage if they care.
}

std::shared_ptr<HitNode> HitParser::parseString(const std::string& text, std::string& errorMessage)
{
  auto root = std::make_shared<HitNode>();
  root->name.clear();

  std::vector<Token> tokens = tokenize(text);
  size_t pos = 0;
  parseBlockBody(tokens, pos, root, errorMessage);
  return root;
}

std::shared_ptr<HitNode> HitParser::parseFile(const std::string& filePath, std::string& errorMessage)
{
  QFile file(QString::fromStdString(filePath));
  if (!file.open(QIODevice::ReadOnly | QIODevice::Text))
  {
    errorMessage = "Could not open file: " + filePath;
    return nullptr;
  }
  QTextStream in(&file);
  std::string text = in.readAll().toStdString();
  return parseString(text, errorMessage);
}

// ---------------------------------------------------------------------------
// Serialization
// ---------------------------------------------------------------------------
std::string HitParser::quoteIfNeeded(const std::string& value)
{
  bool needsQuote = value.empty();
  for (char c : value)
  {
    if (std::isspace(static_cast<unsigned char>(c)))
    {
      needsQuote = true;
      break;
    }
  }
  if (!needsQuote)
  {
    return value;
  }
  std::string out = "'";
  out += value;
  out += "'";
  return out;
}

void HitParser::serializeNode(const std::shared_ptr<HitNode>& node, int indent, std::string& out)
{
  const std::string pad(indent, ' ');

  if (!node->name.empty())
  {
    out += pad + "[" + node->name + "]\n";
  }

  for (const auto& p : node->params)
  {
    out += pad + "  " + p.name + " = " + quoteIfNeeded(p.value) + "\n";
  }

  for (const auto& child : node->children)
  {
    serializeNode(child, node->name.empty() ? indent : indent + 2, out);
  }

  if (!node->name.empty())
  {
    out += pad + "[]\n";
  }
}

std::string HitParser::serialize(const std::shared_ptr<HitNode>& root)
{
  std::string out;
  serializeNode(root, 0, out);
  return out;
}
