// MooseSchema.h
//
// Loads and queries MOOSE's `--json` syntax dump (the same introspection
// data Peacock itself uses) so the editor can offer:
//   - a dropdown of valid `type = ...` values, scoped to the top-level
//     block being edited (e.g. Kernels -> Diffusion, TimeDerivative, ...)
//   - tooltips and, where the schema declares a fixed set of options
//     (MooseEnum-style parameters), dropdowns for individual parameters
//
// NOTE: This is a best-effort reader, not a strict schema validator. The
// exact nesting of MOOSE's JSON dump varies by block ("star" wildcard
// entries, "subblock_types", "actions", etc.), so rather than reconstruct
// the full block-path hierarchy, this walks the entire JSON tree looking
// for two well-known keys anywhere in it:
//   - "subblock_types": { TypeName: { "parameters": {...}, ... }, ... }
//     recorded against whichever top-level block (e.g. "Kernels") the
//     walk was under when it found it.
//   - "parameters": { paramName: { "description": ..., "options": ... } }
//     recorded into a single global (not block-scoped) parameter lookup.
// That's a simplification: parameter docs/options are looked up by name
// across the whole schema rather than by exact block path. In practice
// MOOSE parameter names are specific enough (e.g. "diffusivity",
// "boundary") that this is still useful; treat it as a convenience layer,
// not ground truth.

#pragma once

#include <QMap>
#include <QString>
#include <QStringList>

struct MooseParamInfo
{
  QString description;
  QString cppType;
  QStringList options; // non-empty for MooseEnum-style fixed-choice params
};

class MooseSchema
{
public:
  bool isLoaded() const { return this->Loaded; }

  // Parse raw stdout from `<app> --json` (handles the
  // **START JSON DATA** / **END JSON DATA** wrapper MOOSE emits, if
  // present).
  bool loadFromAppOutput(const QString& output, QString& errorMessage);

  // Valid "type = " values for children of the given top-level block name
  // (e.g. "Kernels", "BCs", "Mesh"). Empty if unknown.
  QStringList typesForTopBlock(const QString& topBlockName) const;

  // Info for a parameter name, looked up globally (see class-level note
  // above on why this isn't block-scoped). Returns false if unknown.
  bool paramInfo(const QString& paramName, MooseParamInfo& infoOut) const;

private:
  void walk(const class QJsonValue& value, const QString& topBlockName, bool atTopLevel);
  void recordSubblockTypes(const class QJsonObject& subblockTypes, const QString& topBlockName);
  void recordParameters(const class QJsonObject& parameters);

  QMap<QString, QStringList> TypesByTopBlock;
  QMap<QString, MooseParamInfo> ParamsByName;
  bool Loaded = false;
};
