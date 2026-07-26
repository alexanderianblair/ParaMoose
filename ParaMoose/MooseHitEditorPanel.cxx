// MooseHitEditorPanel.cxx
#include "MooseHitEditorPanel.h"
#include "HitParser.h"

// ParaView / pqCore
#include "pqActiveObjects.h"
#include "pqApplicationCore.h"
#include "pqObjectBuilder.h"
#include "pqPipelineSource.h"
#include "pqServer.h"

#include <QAction>
#include <QComboBox>
#include <QFile>
#include <QFileDialog>
#include <QFileInfo>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QInputDialog>
#include <QLabel>
#include <QLineEdit>
#include <QMenu>
#include <QMessageBox>
#include <QProcess>
#include <QPoint>
#include <QPushButton>
#include <QSplitter>
#include <QStringList>
#include <QTableWidget>
#include <QToolBar>
#include <QTreeWidget>
#include <QVBoxLayout>
#include <QVariant>
#include <QWidget>

#include <algorithm>

namespace
{
const int NameColumn = 0;
const int ValueColumn = 1;
}

MooseHitEditorPanel::MooseHitEditorPanel(const QString& title, QWidget* parentWidget)
  : Superclass(title, parentWidget)
{
  this->buildUi();
}

MooseHitEditorPanel::MooseHitEditorPanel(QWidget* parentWidget)
  : Superclass(parentWidget)
{
  this->setWindowTitle("MOOSE Input Editor");
  this->buildUi();
}

MooseHitEditorPanel::~MooseHitEditorPanel() = default;

void MooseHitEditorPanel::buildUi()
{
  QWidget* container = new QWidget(this);
  QVBoxLayout* mainLayout = new QVBoxLayout(container);
  mainLayout->setContentsMargins(4, 4, 4, 4);

  // --- Toolbar -------------------------------------------------------
  QToolBar* toolbar = new QToolBar(container);
  QAction* openAction = toolbar->addAction(tr("Open..."));
  this->SaveAction = toolbar->addAction(tr("Save"));
  QAction* saveAsAction = toolbar->addAction(tr("Save As..."));
  toolbar->addSeparator();
  this->RunAction = toolbar->addAction(tr("Run + Load Result"));
  toolbar->addSeparator();
  QAction* loadSchemaAction = toolbar->addAction(tr("Load App Schema..."));
  this->SaveAction->setEnabled(false);
  this->RunAction->setEnabled(false);

  connect(openAction, &QAction::triggered, this, &MooseHitEditorPanel::onOpenFile);
  connect(this->SaveAction, &QAction::triggered, this, &MooseHitEditorPanel::onSaveFile);
  connect(saveAsAction, &QAction::triggered, this, &MooseHitEditorPanel::onSaveFileAs);
  connect(this->RunAction, &QAction::triggered, this, &MooseHitEditorPanel::onRunSolve);
  connect(loadSchemaAction, &QAction::triggered, this, &MooseHitEditorPanel::onLoadSchema);

  mainLayout->addWidget(toolbar);

  // Path to the compiled MOOSE application executable used by "Run" and
  // by "Load App Schema..." (which runs `<exe> --json`).
  QWidget* execRow = new QWidget(container);
  QHBoxLayout* execLayout = new QHBoxLayout(execRow);
  execLayout->setContentsMargins(0, 0, 0, 0);
  execLayout->addWidget(new QLabel(tr("App executable:"), execRow));
  this->ExecutablePathEdit = new QLineEdit(execRow);
  this->ExecutablePathEdit->setPlaceholderText(tr("/path/to/your-app-opt"));
  execLayout->addWidget(this->ExecutablePathEdit);
  mainLayout->addWidget(execRow);

  this->SchemaStatusLabel = new QLabel(tr("Schema: not loaded (type dropdowns/tooltips unavailable)"), container);
  this->SchemaStatusLabel->setStyleSheet("color: gray; font-style: italic;");
  mainLayout->addWidget(this->SchemaStatusLabel);

  // --- Tree + parameter table -----------------------------------------
  QSplitter* splitter = new QSplitter(Qt::Horizontal, container);

  this->Tree = new QTreeWidget(splitter);
  this->Tree->setHeaderLabels(QStringList() << tr("Block"));
  this->Tree->setContextMenuPolicy(Qt::CustomContextMenu);
  connect(this->Tree, &QTreeWidget::itemSelectionChanged, this, &MooseHitEditorPanel::onTreeSelectionChanged);
  connect(
    this->Tree, &QTreeWidget::customContextMenuRequested, this, &MooseHitEditorPanel::onTreeContextMenu);

  QWidget* rightPane = new QWidget(splitter);
  QVBoxLayout* rightLayout = new QVBoxLayout(rightPane);
  rightLayout->setContentsMargins(0, 0, 0, 0);

  this->ParamTable = new QTableWidget(0, 2, rightPane);
  this->ParamTable->setHorizontalHeaderLabels(QStringList() << tr("Parameter") << tr("Value"));
  this->ParamTable->horizontalHeader()->setStretchLastSection(true);
  connect(this->ParamTable, &QTableWidget::cellChanged, this, &MooseHitEditorPanel::onParamTableCellChanged);
  rightLayout->addWidget(this->ParamTable);

  QWidget* paramButtonRow = new QWidget(rightPane);
  QHBoxLayout* paramButtonLayout = new QHBoxLayout(paramButtonRow);
  paramButtonLayout->setContentsMargins(0, 0, 0, 0);
  QPushButton* addParamButton = new QPushButton(tr("Add Parameter"), paramButtonRow);
  QPushButton* removeParamButton = new QPushButton(tr("Remove Selected"), paramButtonRow);
  connect(addParamButton, &QPushButton::clicked, this, &MooseHitEditorPanel::onAddParamRow);
  connect(removeParamButton, &QPushButton::clicked, this, &MooseHitEditorPanel::onRemoveParamRow);
  paramButtonLayout->addWidget(addParamButton);
  paramButtonLayout->addWidget(removeParamButton);
  paramButtonLayout->addStretch();
  rightLayout->addWidget(paramButtonRow);

  splitter->addWidget(this->Tree);
  splitter->addWidget(rightPane);
  splitter->setStretchFactor(0, 1);
  splitter->setStretchFactor(1, 2);

  mainLayout->addWidget(splitter, 1);

  this->setWidget(container);
  this->updateWindowTitleForFile();
}

// ---------------------------------------------------------------------------
// File I/O
// ---------------------------------------------------------------------------
void MooseHitEditorPanel::onOpenFile()
{
  if (this->Dirty)
  {
    auto reply = QMessageBox::question(this, tr("Discard changes?"),
      tr("The current input has unsaved changes. Open a new file anyway?"));
    if (reply != QMessageBox::Yes)
    {
      return;
    }
  }

  QString filePath = QFileDialog::getOpenFileName(
    this, tr("Open MOOSE Input File"), QString(), tr("MOOSE input files (*.i);;All files (*)"));
  if (filePath.isEmpty())
  {
    return;
  }
  this->loadFile(filePath);
}

void MooseHitEditorPanel::loadFile(const QString& filePath)
{
  std::string error;
  auto root = HitParser::parseFile(filePath.toStdString(), error);
  if (!root)
  {
    QMessageBox::warning(this, tr("Failed to open file"), QString::fromStdString(error));
    return;
  }
  if (!error.empty())
  {
    QMessageBox::warning(this, tr("Parsed with warnings"), QString::fromStdString(error));
  }

  this->RootNode = root;
  this->CurrentFilePath = filePath;
  this->rebuildTree();
  this->markDirty(false);
  this->SaveAction->setEnabled(true);
  this->RunAction->setEnabled(true);
  this->updateWindowTitleForFile();
}

void MooseHitEditorPanel::onSaveFile()
{
  if (this->CurrentFilePath.isEmpty())
  {
    this->onSaveFileAs();
    return;
  }
  this->saveToPath(this->CurrentFilePath);
}

void MooseHitEditorPanel::onSaveFileAs()
{
  QString filePath = QFileDialog::getSaveFileName(
    this, tr("Save MOOSE Input File"), this->CurrentFilePath, tr("MOOSE input files (*.i);;All files (*)"));
  if (filePath.isEmpty())
  {
    return;
  }
  this->saveToPath(filePath);
}

void MooseHitEditorPanel::saveToPath(const QString& filePath)
{
  if (!this->RootNode)
  {
    return;
  }
  std::string text = HitParser::serialize(this->RootNode);

  QFile file(filePath);
  if (!file.open(QIODevice::WriteOnly | QIODevice::Text))
  {
    QMessageBox::warning(this, tr("Failed to save file"), tr("Could not open %1 for writing.").arg(filePath));
    return;
  }
  file.write(text.c_str());
  file.close();

  this->CurrentFilePath = filePath;
  this->markDirty(false);
  this->SaveAction->setEnabled(true);
  this->RunAction->setEnabled(true);
  this->updateWindowTitleForFile();
}

// ---------------------------------------------------------------------------
// Run the solver, then load the Exodus result into the active ParaView
// pipeline. This is the piece that ties the input-editing side of the
// panel to ParaView's existing visualization machinery: once the .e file
// exists, ParaView's own Exodus reader takes over.
// ---------------------------------------------------------------------------
void MooseHitEditorPanel::onRunSolve()
{
  if (this->CurrentFilePath.isEmpty())
  {
    QMessageBox::information(this, tr("Save first"), tr("Save the input file before running."));
    return;
  }
  QString exe = this->ExecutablePathEdit->text().trimmed();
  if (exe.isEmpty() || !QFileInfo::exists(exe))
  {
    QMessageBox::warning(
      this, tr("No executable"), tr("Set a valid path to your MOOSE application executable first."));
    return;
  }

  QFileInfo inputInfo(this->CurrentFilePath);
  QStringList args;
  args << "-i" << inputInfo.fileName();

  QProcess process;
  process.setWorkingDirectory(inputInfo.absolutePath());
  process.setProgram(exe);
  process.setArguments(args);
  process.setProcessChannelMode(QProcess::MergedChannels);

  process.start();
  if (!process.waitForStarted())
  {
    QMessageBox::warning(this, tr("Run failed"), tr("Could not start %1").arg(exe));
    return;
  }
  // NOTE: for a real plugin, prefer running this asynchronously and
  // streaming output to a log widget (or a pqOutputWidget) instead of
  // blocking the UI thread with waitForFinished(). Kept synchronous here
  // to keep the example short.
  process.waitForFinished(-1);

  QString output = QString::fromLocal8Bit(process.readAll());
  if (process.exitStatus() != QProcess::NormalExit || process.exitCode() != 0)
  {
    QMessageBox::warning(this, tr("Solve failed"), output);
    return;
  }

  // MOOSE's default output naming is "<input-file-stem>_out.e".
  QString resultPath =
    inputInfo.absolutePath() + "/" + inputInfo.completeBaseName() + "_out.e";
  if (!QFileInfo::exists(resultPath))
  {
    QMessageBox::information(this, tr("Solve finished"),
      tr("Run completed but the expected result file was not found:\n%1").arg(resultPath));
    return;
  }

  pqObjectBuilder* builder = pqApplicationCore::instance()->getObjectBuilder();
  pqServer* server = pqActiveObjects::instance().activeServer();
  pqPipelineSource* source =
    builder->createReader("sources", "ExodusIIReader", QStringList(resultPath), server);
  if (source)
  {
    source->updatePipeline();
  }
}

// ---------------------------------------------------------------------------
// Schema awareness: run `<exe> --json` and parse MOOSE's syntax dump so the
// parameter table can offer type dropdowns / enum dropdowns / tooltips.
// ---------------------------------------------------------------------------
void MooseHitEditorPanel::onLoadSchema()
{
  QString exe = this->ExecutablePathEdit->text().trimmed();
  if (exe.isEmpty() || !QFileInfo::exists(exe))
  {
    QMessageBox::warning(
      this, tr("No executable"), tr("Set a valid path to your MOOSE application executable first."));
    return;
  }

  QProcess process;
  process.setProgram(exe);
  process.setArguments(QStringList() << "--json");
  process.setProcessChannelMode(QProcess::MergedChannels);
  process.start();
  if (!process.waitForStarted())
  {
    QMessageBox::warning(this, tr("Failed to run"), tr("Could not start %1").arg(exe));
    return;
  }
  process.waitForFinished(-1);

  QString output = QString::fromUtf8(process.readAll());
  QString error;
  if (!this->Schema.loadFromAppOutput(output, error))
  {
    QMessageBox::warning(this, tr("Failed to parse schema"), error);
    this->SchemaStatusLabel->setText(tr("Schema: failed to load (%1)").arg(error));
    return;
  }

  this->SchemaStatusLabel->setText(tr("Schema: loaded from %1 --json").arg(QFileInfo(exe).fileName()));
  this->SchemaStatusLabel->setStyleSheet("color: green;");

  // Refresh the currently displayed block, if any, so dropdowns/tooltips
  // appear immediately without needing to re-click the tree.
  QList<QTreeWidgetItem*> selected = this->Tree->selectedItems();
  if (!selected.isEmpty())
  {
    this->populateParamTableFor(this->nodeForItem(selected.first()));
  }
}

QString MooseHitEditorPanel::topLevelBlockName(HitNode* node) const
{
  if (!node)
  {
    return QString();
  }
  while (node->parent && node->parent->parent)
  {
    node = node->parent;
  }
  return QString::fromStdString(node->name);
}

// ---------------------------------------------------------------------------
// Tree <-> HitNode model
// ---------------------------------------------------------------------------
void MooseHitEditorPanel::rebuildTree()
{
  this->Tree->clear();
  if (!this->RootNode)
  {
    return;
  }
  for (const auto& child : this->RootNode->children)
  {
    this->addTreeItemsRecursive(nullptr, child);
  }
  this->Tree->expandAll();
}

void MooseHitEditorPanel::addTreeItemsRecursive(QTreeWidgetItem* parentItem, const std::shared_ptr<HitNode>& node)
{
  QTreeWidgetItem* item = parentItem ? new QTreeWidgetItem(parentItem) : new QTreeWidgetItem(this->Tree);
  item->setText(0, QString::fromStdString(node->name));
  item->setData(0, Qt::UserRole, QVariant::fromValue(node.get()));

  for (const auto& child : node->children)
  {
    this->addTreeItemsRecursive(item, child);
  }
}

HitNode* MooseHitEditorPanel::nodeForItem(QTreeWidgetItem* item) const
{
  if (!item)
  {
    return nullptr;
  }
  return item->data(0, Qt::UserRole).value<HitNode*>();
}

void MooseHitEditorPanel::onTreeSelectionChanged()
{
  QList<QTreeWidgetItem*> selected = this->Tree->selectedItems();
  HitNode* node = selected.isEmpty() ? nullptr : this->nodeForItem(selected.first());
  this->populateParamTableFor(node);
}

void MooseHitEditorPanel::populateParamTableFor(HitNode* node)
{
  this->BlockTableSignals = true;
  this->ParamTable->setRowCount(0);

  if (!node)
  {
    this->BlockTableSignals = false;
    return;
  }

  const QString topBlock = this->topLevelBlockName(node);
  const QStringList typeChoices = this->Schema.typesForTopBlock(topBlock);

  for (const auto& p : node->params)
  {
    QString paramName = QString::fromStdString(p.name);
    QString paramValue = QString::fromStdString(p.value);

    int row = this->ParamTable->rowCount();
    this->ParamTable->insertRow(row);

    QTableWidgetItem* nameItem = new QTableWidgetItem(paramName);
    MooseParamInfo info;
    QStringList options = (paramName == "type") ? typeChoices : QStringList();
    if (this->Schema.paramInfo(p.name.c_str(), info))
    {
      QString tip = info.description;
      if (!info.cppType.isEmpty())
      {
        tip += tip.isEmpty() ? QString() : "\n";
        tip += tr("Type: %1").arg(info.cppType);
      }
      if (!tip.isEmpty())
      {
        nameItem->setToolTip(tip);
      }
      if (options.isEmpty() && !info.options.isEmpty())
      {
        options = info.options;
      }
    }
    this->ParamTable->setItem(row, NameColumn, nameItem);

    if (!options.isEmpty())
    {
      // Schema-backed dropdown: valid `type = ` values for this block, or
      // a MooseEnum-style fixed set of options for this parameter.
      // Editable so the user can still type a value the schema doesn't
      // know about (e.g. a newer type than what --json reported).
      QComboBox* combo = new QComboBox(this->ParamTable);
      combo->setEditable(true);
      combo->addItems(options);
      int existingIndex = options.indexOf(paramValue);
      if (existingIndex >= 0)
      {
        combo->setCurrentIndex(existingIndex);
      }
      else
      {
        combo->addItem(paramValue);
        combo->setCurrentIndex(combo->count() - 1);
      }
      // node/p captured by pointer+index: params vector is stable for the
      // lifetime of this table (rows are rebuilt wholesale on any
      // structural change), so capturing the row index by value is safe.
      connect(combo, &QComboBox::currentTextChanged, this,
        [this, node, row](const QString& text) {
          if (row < 0 || row >= static_cast<int>(node->params.size()))
          {
            return;
          }
          node->params[row].value = text.toStdString();
          this->markDirty(true);
        });
      this->ParamTable->setCellWidget(row, ValueColumn, combo);
    }
    else
    {
      this->ParamTable->setItem(row, ValueColumn, new QTableWidgetItem(paramValue));
    }
  }

  this->BlockTableSignals = false;
}

void MooseHitEditorPanel::onTreeContextMenu(const QPoint& pos)
{
  QMenu menu(this);
  QAction* addAction = menu.addAction(tr("Add Child Block"));
  QAction* removeAction = menu.addAction(tr("Remove Block"));
  removeAction->setEnabled(!this->Tree->selectedItems().isEmpty());

  QAction* chosen = menu.exec(this->Tree->viewport()->mapToGlobal(pos));
  if (chosen == addAction)
  {
    this->onAddChildBlock();
  }
  else if (chosen == removeAction)
  {
    this->onRemoveBlock();
  }
}

void MooseHitEditorPanel::onAddChildBlock()
{
  if (!this->RootNode)
  {
    return;
  }
  bool ok = false;
  QString name = QInputDialog::getText(this, tr("New Block"), tr("Block name:"), QLineEdit::Normal, QString(), &ok);
  if (!ok || name.trimmed().isEmpty())
  {
    return;
  }

  QList<QTreeWidgetItem*> selected = this->Tree->selectedItems();
  HitNode* parentNode = selected.isEmpty() ? this->RootNode.get() : this->nodeForItem(selected.first());
  QTreeWidgetItem* parentItem = selected.isEmpty() ? nullptr : selected.first();

  auto child = std::make_shared<HitNode>();
  child->name = name.trimmed().toStdString();
  child->parent = parentNode;

  // Find the shared_ptr owner in parentNode's children to attach to -- for
  // the root this means pushing onto RootNode->children directly.
  if (parentNode == this->RootNode.get())
  {
    this->RootNode->children.push_back(child);
  }
  else
  {
    // parentNode is a raw pointer into the tree; its owning shared_ptr is
    // held by *its* parent's children vector, so we can push directly onto
    // parentNode->children since HitNode owns its own children vector.
    parentNode->children.push_back(child);
  }

  this->addTreeItemsRecursive(parentItem, child);
  this->markDirty(true);
}

void MooseHitEditorPanel::onRemoveBlock()
{
  QList<QTreeWidgetItem*> selected = this->Tree->selectedItems();
  if (selected.isEmpty())
  {
    return;
  }
  QTreeWidgetItem* item = selected.first();
  HitNode* node = this->nodeForItem(item);
  if (!node || !node->parent)
  {
    return;
  }

  HitNode* parent = node->parent;
  auto& siblings = parent->children;
  siblings.erase(
    std::remove_if(siblings.begin(), siblings.end(), [node](const std::shared_ptr<HitNode>& n) { return n.get() == node; }),
    siblings.end());

  delete item;
  this->markDirty(true);
}

// ---------------------------------------------------------------------------
// Parameter table <-> HitNode params
// ---------------------------------------------------------------------------
void MooseHitEditorPanel::onParamTableCellChanged(int row, int column)
{
  if (this->BlockTableSignals)
  {
    return;
  }
  QList<QTreeWidgetItem*> selected = this->Tree->selectedItems();
  if (selected.isEmpty())
  {
    return;
  }
  HitNode* node = this->nodeForItem(selected.first());
  if (!node || row < 0 || row >= static_cast<int>(node->params.size()))
  {
    return;
  }

  QTableWidgetItem* nameItem = this->ParamTable->item(row, NameColumn);
  if (!nameItem)
  {
    return;
  }
  node->params[row].name = nameItem->text().toStdString();

  // Rows whose value is a schema-backed dropdown have no QTableWidgetItem
  // in the value column (it's a QComboBox instead); those update
  // node->params[row].value themselves via the combo's connected lambda.
  QTableWidgetItem* valueItem = this->ParamTable->item(row, ValueColumn);
  if (valueItem)
  {
    node->params[row].value = valueItem->text().toStdString();
  }
  (void)column;
  this->markDirty(true);
}

void MooseHitEditorPanel::onAddParamRow()
{
  QList<QTreeWidgetItem*> selected = this->Tree->selectedItems();
  if (selected.isEmpty())
  {
    QMessageBox::information(this, tr("Select a block"), tr("Select a block in the tree first."));
    return;
  }
  HitNode* node = this->nodeForItem(selected.first());
  if (!node)
  {
    return;
  }
  node->params.push_back({ "new_param", "value" });
  this->populateParamTableFor(node);
  this->markDirty(true);
}

void MooseHitEditorPanel::onRemoveParamRow()
{
  QList<QTreeWidgetItem*> selected = this->Tree->selectedItems();
  int row = this->ParamTable->currentRow();
  if (selected.isEmpty() || row < 0)
  {
    return;
  }
  HitNode* node = this->nodeForItem(selected.first());
  if (!node || row >= static_cast<int>(node->params.size()))
  {
    return;
  }
  node->params.erase(node->params.begin() + row);
  this->populateParamTableFor(node);
  this->markDirty(true);
}

// ---------------------------------------------------------------------------
void MooseHitEditorPanel::markDirty(bool dirty)
{
  this->Dirty = dirty;
  this->updateWindowTitleForFile();
}

void MooseHitEditorPanel::updateWindowTitleForFile()
{
  QString base = this->CurrentFilePath.isEmpty() ? tr("(no file)") : QFileInfo(this->CurrentFilePath).fileName();
  this->setWindowTitle(tr("MOOSE Input Editor - %1%2").arg(base, this->Dirty ? "*" : ""));
}
