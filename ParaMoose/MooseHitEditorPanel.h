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
//   [ tree of blocks (left) | parameter table for selected block (right) ]

#pragma once

#include "MooseSchema.h"

#include <QDockWidget>

class QTreeWidget;
class QTreeWidgetItem;
class QTableWidget;
class QAction;
class QLineEdit;
class QLabel;

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
  void onAddParamRow();
  void onRemoveParamRow();

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

  QTreeWidget* Tree = nullptr;
  QTableWidget* ParamTable = nullptr;
  QAction* SaveAction = nullptr;
  QAction* RunAction = nullptr;
  QAction* ViewMeshAction = nullptr;
  QLineEdit* ExecutablePathEdit = nullptr;
  QLabel* SchemaStatusLabel = nullptr;

  // Owning pointer to the root of the currently-open hit tree. hit::Node's
  // destructor recursively deletes its children (and safely unlinks
  // itself from its parent, confirmed against MOOSE's own
  // framework/contrib/hit source), so this is the single point of
  // ownership -- delete it whenever replacing it, and in the destructor.
  hit::Node* RootNode = nullptr;
  QString CurrentFilePath;
  bool Dirty = false;

  MooseSchema Schema;

  // Guards against onParamTableCellChanged reacting to programmatic
  // updates (e.g. while repopulating the table after a tree selection
  // change).
  bool BlockTableSignals = false;

  Q_DISABLE_COPY(MooseHitEditorPanel)
};
