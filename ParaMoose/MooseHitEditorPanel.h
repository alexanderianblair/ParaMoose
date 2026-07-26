// MooseHitEditorPanel.h
//
// A ParaView dock-widget panel that opens, edits, and saves MOOSE "hit"
// input files (.i) from within the ParaView GUI, using MOOSE's own hit
// parser/renderer (framework/contrib/hit) rather than a hand-rolled one --
// see MooseHitEditorPanel.cxx and the top-level README for what that buys
// you (full grammar support, correct quoting/escaping, and formatting
// fidelity on save) and what it still doesn't (in-place comment editing;
// comments outside edited regions are preserved by render(), but this UI
// has no way to add/see them).
//
// Layout:
//   [ toolbar: New | Open | Save | Save As | Run | View Mesh | Load Schema ]
//   [ tabs: "Structured" (tree + parameter table) | "Raw Text" (editable
//     render() preview, synced explicitly via Apply/Refresh, not live) ]
//   [ pqOutputWidget: streamed solver output from "Run + Load Result" ]

#pragma once

#include "MooseSchema.h"

#include <QDockWidget>

class QTreeWidget;
class QTreeWidgetItem;
class QTableWidget;
class QAction;
class QLineEdit;
class QLabel;
class QProcess;
class QTabWidget;
class QPlainTextEdit;
class pqOutputWidget;
class pqPipelineSource;
class pqDataRepresentation;

namespace hit
{
class Node;
}

class MooseHitEditorPanel : public QDockWidget
{
  Q_OBJECT
  typedef QDockWidget Superclass;

public:
  // ParaView's dock-window machinery expects this exact constructor
  // signature (title + parent) so it can instantiate the panel and place
  // it in the main window.
  explicit MooseHitEditorPanel(const QString& title, QWidget* parent = nullptr);
  explicit MooseHitEditorPanel(QWidget* parent = nullptr);
  ~MooseHitEditorPanel() override;

private slots:
  void onNewFile();
  void onOpenFile();
  void onSaveFile();
  void onSaveFileAs();
  void onRunSolve();
  void onLoadSchema();
  void onViewMesh();

  void onTreeSelectionChanged();
  void onTreeContextMenu(const QPoint& pos);
  void onAddTopLevelBlock();
  void onRemoveBlock();

  void onParamTableCellChanged(int row, int column);
  void onParamTableCurrentCellChanged(int currentRow, int currentColumn, int previousRow, int previousColumn);
  void onAddParamRow();
  void onRemoveParamRow();

  void onMainTabChanged(int index);
  void onApplyRawText();
  void onRefreshRawTextFromModel();

private:
  void buildUi();
  void resetToNewRoot();
  void setRootFromParsedText(const QString& fname, const QString& text);
  void loadFile(const QString& filePath);
  void saveToPath(const QString& filePath);
  void rebuildTree();
  void addTreeItemsRecursive(QTreeWidgetItem* parentItem, hit::Node* node);
  void addBlockUnder(hit::Node* parentNode, QTreeWidgetItem* parentItem);
  void populateParamTableFor(hit::Node* node);
  hit::Node* nodeForItem(QTreeWidgetItem* item) const;
  void markDirty(bool dirty);
  void updateWindowTitleForFile();
  QString topLevelBlockName(hit::Node* node) const;
  QString findMeshFile() const;
  QString resolveInputRelativePath(const QString& maybeRelative) const;
  void ensureHighlightSource(const QString& meshFilePath);
  void updateHighlight(hit::Node* fieldNode, const QString& paramName);
  void clearHighlight();
  void refreshRawTextFromModel();

  QTreeWidget* Tree = nullptr;
  QTableWidget* ParamTable = nullptr;
  QAction* SaveAction = nullptr;
  QAction* RunAction = nullptr;
  QAction* ViewMeshAction = nullptr;
  QLineEdit* ExecutablePathEdit = nullptr;
  QLabel* SchemaStatusLabel = nullptr;
  pqOutputWidget* OutputWidget = nullptr;
  QTabWidget* MainTabs = nullptr;
  QPlainTextEdit* RawTextEdit = nullptr;
  int RawTextTabIndex = -1;

  // The text most recently written into RawTextEdit by this code (either
  // from refreshRawTextFromModel() or right after a successful Apply).
  // Comparing RawTextEdit's current text against this is how tab-switch
  // auto-refresh and the Refresh button decide whether there are
  // unapplied edits it would be destroying -- see onMainTabChanged() and
  // onRefreshRawTextFromModel().
  QString LastSyncedRawText;

  // Non-null only while a solve launched via onRunSolve() is in flight.
  // Owned via QObject parenting (constructed with `this` as parent) but
  // also explicitly deleteLater()'d on completion; see onRunSolve().
  QProcess* SolveProcess = nullptr;

  // Owning pointer to the root of the currently-open hit tree. hit::Node's
  // destructor recursively deletes its children (and safely unlinks
  // itself from its parent, confirmed against MOOSE's own
  // framework/contrib/hit source), so this is the single point of
  // ownership -- delete it whenever replacing it, and in the destructor.
  hit::Node* RootNode = nullptr;
  QString CurrentFilePath;
  bool Dirty = false;

  MooseSchema Schema;

  // File path of whatever mesh/result was most recently loaded via
  // onViewMesh()/onRunSolve(), used as the source file for the highlight
  // reader below. Empty if nothing has been loaded yet this session.
  QString LastLoadedMeshFilePath;

  // A second, independent Exodus reader + representation pointed at the
  // same file as LastLoadedMeshFilePath, used purely to highlight a
  // boundary/block by name -- kept separate from whatever the person is
  // actually looking at so toggling a highlight never disturbs their main
  // view's array selection or coloring. Hidden (setVisible(false)) except
  // while a boundary/block parameter row is the current table selection.
  pqPipelineSource* HighlightSource = nullptr;
  pqDataRepresentation* HighlightRepresentation = nullptr;
  QString HighlightSourceFilePath;

  // Guards against onParamTableCellChanged reacting to programmatic
  // updates (e.g. while repopulating the table after a tree selection
  // change).
  bool BlockTableSignals = false;

  Q_DISABLE_COPY(MooseHitEditorPanel)
};
