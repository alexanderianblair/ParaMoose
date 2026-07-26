// HitParser.h
//
// A deliberately small, self-contained parser/serializer for MOOSE's "hit"
// input file format, e.g.:
//
//   [Mesh]
//     type = GeneratedMesh
//     dim = 2
//     nx = 10
//   []
//
//   [Variables]
//     [u]
//     []
//   []
//
// NOTE: This is a simplified reader meant to make the ParaView plugin example
// self-contained and easy to read. It handles the common cases (nested
// blocks, `key = value`, quoted string/vector values, `#` comments) but does
// NOT implement the full hit grammar (no !include, no multi-line strings,
// no inline blocks-on-one-line, limited escaping). For production use, link
// against MOOSE's own hit library (framework/contrib/hit in the MOOSE
// repository) instead of this parser -- it is the parser MOOSE itself uses,
// so anything it accepts is guaranteed to round-trip correctly.

#pragma once

#include <QMetaType>
#include <memory>
#include <string>
#include <vector>

struct HitParam
{
  std::string name;
  std::string value;
};

struct HitNode
{
  // Empty string for the implicit root node.
  std::string name;
  std::vector<HitParam> params;
  std::vector<std::shared_ptr<HitNode>> children;
  HitNode* parent = nullptr;

  // Convenience: full dotted path from root, e.g. "Variables/u".
  std::string path() const;
};

class HitParser
{
public:
  // Parse a .i/.hit file from disk. Returns nullptr on failure and fills
  // errorMessage.
  static std::shared_ptr<HitNode> parseFile(const std::string& filePath, std::string& errorMessage);

  // Parse hit-format text directly (used by parseFile, exposed for testing).
  static std::shared_ptr<HitNode> parseString(const std::string& text, std::string& errorMessage);

  // Serialize a tree back to hit-format text.
  static std::string serialize(const std::shared_ptr<HitNode>& root);

private:
  struct Token
  {
    enum class Kind
    {
      BlockOpen,  // [Name]
      BlockClose, // []
      Identifier, // bare word (key or value)
      String      // 'quoted value'
    };
    Kind kind;
    std::string text;
    int line;
  };

  static std::vector<Token> tokenize(const std::string& text);
  static void parseBlockBody(const std::vector<Token>& tokens, size_t& pos, const std::shared_ptr<HitNode>& node,
    std::string& errorMessage);
  static void serializeNode(const std::shared_ptr<HitNode>& node, int indent, std::string& out);
  static std::string quoteIfNeeded(const std::string& value);
};

Q_DECLARE_METATYPE(HitNode*)
