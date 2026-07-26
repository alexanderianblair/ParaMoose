// MooseSchema.cxx
#include "MooseSchema.h"

#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonValue>

bool MooseSchema::loadFromAppOutput(const QString& output, QString& errorMessage)
{
  // MOOSE wraps the JSON payload between sentinel lines so it can be
  // embedded in normal stdout alongside other startup chatter:
  //   **START JSON DATA**
  //   { ... }
  //   **END JSON DATA**
  // Fall back to using the whole output if the markers aren't found, in
  // case a given app/version prints only the JSON.
  QString jsonText = output;
  const QString startMarker = "**START JSON DATA**";
  const QString endMarker = "**END JSON DATA**";
  int start = output.indexOf(startMarker);
  int end = output.indexOf(endMarker);
  if (start >= 0 && end > start)
  {
    jsonText = output.mid(start + startMarker.length(), end - (start + startMarker.length()));
  }

  QJsonParseError parseError;
  QJsonDocument doc = QJsonDocument::fromJson(jsonText.trimmed().toUtf8(), &parseError);
  if (doc.isNull())
  {
    errorMessage = "Failed to parse JSON: " + parseError.errorString();
    return false;
  }
  if (!doc.isObject())
  {
    errorMessage = "Expected a JSON object at the top level of the syntax dump.";
    return false;
  }

  this->TypesByTopBlock.clear();
  this->ParamsByName.clear();

  QJsonObject root = doc.object();
  // Most MOOSE apps nest everything under a top-level "blocks" key; fall
  // back to treating the root itself as the blocks map if that key is
  // absent, since this has shifted across MOOSE versions.
  QJsonObject blocks = root.contains("blocks") && root.value("blocks").isObject()
    ? root.value("blocks").toObject()
    : root;

  for (auto it = blocks.begin(); it != blocks.end(); ++it)
  {
    this->walk(it.value(), it.key(), true);
  }

  this->Loaded = true;
  return true;
}

void MooseSchema::recordSubblockTypes(const QJsonObject& subblockTypes, const QString& topBlockName)
{
  QStringList types = this->TypesByTopBlock.value(topBlockName);
  for (auto it = subblockTypes.begin(); it != subblockTypes.end(); ++it)
  {
    if (!types.contains(it.key()))
    {
      types << it.key();
    }
    // Each type's own entry typically carries its own "parameters" too.
    if (it.value().isObject())
    {
      QJsonObject typeObj = it.value().toObject();
      if (typeObj.contains("parameters") && typeObj.value("parameters").isObject())
      {
        this->recordParameters(typeObj.value("parameters").toObject());
      }
    }
  }
  types.sort(Qt::CaseInsensitive);
  this->TypesByTopBlock.insert(topBlockName, types);
}

void MooseSchema::recordParameters(const QJsonObject& parameters)
{
  for (auto it = parameters.begin(); it != parameters.end(); ++it)
  {
    if (!it.value().isObject())
    {
      continue;
    }
    QJsonObject paramObj = it.value().toObject();

    MooseParamInfo info = this->ParamsByName.value(it.key());
    if (info.description.isEmpty() && paramObj.contains("description"))
    {
      info.description = paramObj.value("description").toString();
    }
    if (info.cppType.isEmpty() && paramObj.contains("cpp_type"))
    {
      info.cppType = paramObj.value("cpp_type").toString();
    }
    if (info.options.isEmpty() && paramObj.contains("options"))
    {
      // MOOSE reports MooseEnum-style options as a single
      // space-separated string.
      QString optionsStr = paramObj.value("options").toString();
      if (!optionsStr.isEmpty())
      {
        info.options = optionsStr.split(' ', Qt::SkipEmptyParts);
      }
    }
    this->ParamsByName.insert(it.key(), info);
  }
}

void MooseSchema::walk(const QJsonValue& value, const QString& topBlockName, bool atTopLevel)
{
  (void)atTopLevel;
  if (value.isObject())
  {
    QJsonObject obj = value.toObject();

    if (obj.contains("subblock_types") && obj.value("subblock_types").isObject())
    {
      this->recordSubblockTypes(obj.value("subblock_types").toObject(), topBlockName);
    }
    if (obj.contains("parameters") && obj.value("parameters").isObject())
    {
      this->recordParameters(obj.value("parameters").toObject());
    }

    for (auto it = obj.begin(); it != obj.end(); ++it)
    {
      this->walk(it.value(), topBlockName, false);
    }
  }
  else if (value.isArray())
  {
    for (const QJsonValue& child : value.toArray())
    {
      this->walk(child, topBlockName, false);
    }
  }
}

QStringList MooseSchema::typesForTopBlock(const QString& topBlockName) const
{
  return this->TypesByTopBlock.value(topBlockName);
}

bool MooseSchema::paramInfo(const QString& paramName, MooseParamInfo& infoOut) const
{
  auto it = this->ParamsByName.find(paramName);
  if (it == this->ParamsByName.end())
  {
    return false;
  }
  infoOut = it.value();
  return true;
}
