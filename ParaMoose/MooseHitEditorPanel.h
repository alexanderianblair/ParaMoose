// MooseHitEditorPanel.h
//
// A ParaView dock-widget panel that opens, edits, and saves MOOSE "hit"
// input files (.i) from within the ParaView GUI.
//
// Layout:
//   [ toolbar: Open | Save | Save As ]
//   [ tree of blocks (left) | parameter table for selected block (right) ]
//
// After saving, the panel can optionally run the associated MOOSE
// executable and load the resulting Exodus file into the active ParaView
// pipeline -- see runSolveAndLoadResult() -- so the same panel drives both
// "edit the input" and "see the result" without leaving ParaView.

#pragma once

#include <QDockWidget>

#include <memory>

class QTreeWidget;
class QTreeWidgetItem;
class QTableWidget;
class QAction;
class QLineEdit;
struct HitNode;

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
  void onOpenFile();
  void onSaveFile();
  void onSaveFileAs();
  void onRunSolve();

  void onTreeSelectionChanged();
  void onTreeContextMenu(const QPoint& pos);
  void onAddChildBlock();
  void onRemoveBlock();

  void onParamTableCellChanged(int row, int column);
  void onAddParamRow();
  void onRemoveParamRow();

private:
  void buildUi();
  void loadFile(const QString& filePath);
  void saveToPath(const QString& filePath);
  void rebuildTree();
  void addTreeItemsRecursive(QTreeWidgetItem* parentItem, const std::shared_ptr<HitNode>& node);
  void populateParamTableFor(const std::shared_ptr<HitNode>& node);
  HitNode* nodeForItem(QTreeWidgetItem* item) const;
  void markDirty(bool dirty);
  void updateWindowTitleForFile();

  QTreeWidget* Tree = nullptr;
  QTableWidget* ParamTable = nullptr;
  QAction* SaveAction = nullptr;
  QAction* RunAction = nullptr;
  QLineEdit* ExecutablePathEdit = nullptr;

  std::shared_ptr<HitNode> RootNode;
  QString CurrentFilePath;
  bool Dirty = false;

  // Guards against onParamTableCellChanged reacting to programmatic
  // updates (e.g. while repopulating the table after a tree selection
  // change).
  bool BlockTableSignals = false;

  Q_DISABLE_COPY(MooseHitEditorPanel)
};
