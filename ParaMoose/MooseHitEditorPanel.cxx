// MooseHitEditorPanel.cxx
#include "MooseHitEditorPanel.h"

// MOOSE's real hit parser (framework/contrib/hit). See CMakeLists.txt for
// how this gets built/linked (it depends on WASP, a separate compiled
// library that MOOSE builds alongside it).
#include "hit/hit.h"

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
#include <QMetaType>
#include <QPoint>
#include <QProcess>
#include <QPushButton>
#include <QSplitter>
#include <QStringList>
#include <QTableWidget>
#include <QTextStream>
#include <QToolBar>
#include <QTreeWidget>
#include <QVBoxLayout>
#include <QVariant>
#include <QWidget>

#include <algorithm>

// Lets QVariant carry raw hit::Node pointers (used to tag tree items and
// parameter-table rows with the tree node they represent). Only used
// within this translation unit, so declaring it here rather than in a
// header is fine.
Q_DECLARE_METATYPE(hit::Node*)

namespace
{
const int NameColumn = 0;
const int ValueColumn = 1;

// Depth-first search for the first Field literally named "file" anywhere
// in node's subtree (its own fields first, then child sections in order).
// Covers both the classic `[Mesh] type = FileMesh file = ...` style and
// newer MeshGenerator-nested styles, since either way the file path shows
// up as a `file = ...` field somewhere under [Mesh].
bool findFileParamRecursive(hit::Node* node, std::string& valueOut)
{
  for (hit::Node* field : node->children(hit::NodeType::Field))
  {
    if (field->path() == "file")
    {
      valueOut = field->strVal();
      return true;
    }
  }
  for (hit::Node* section : node->children(hit::NodeType::Section))
  {
    if (findFileParamRecursive(section, valueOut))
    {
      return true;
    }
  }
  return false;
}
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

MooseHitEditorPanel::~MooseHitEditorPanel()
{
  delete this->RootNode;
}

void MooseHitEditorPanel::buildUi()
{
  QWidget* container = new QWidget(this);
  QVBoxLayout* mainLayout = new QVBoxLayout(container);
  mainLayout->setContentsMargins(4, 4, 4, 4);

  // --- Toolbar -------------------------------------------------------
  QToolBar* toolbar = new QToolBar(container);
  QAction* newAction = toolbar->addAction(tr("New"));
  QAction* openAction = toolbar->addAction(tr("Open..."));
  this->SaveAction = toolbar->addAction(tr("Save"));
  QAction* saveAsAction = toolbar->addAction(tr("Save As..."));
  toolbar->addSeparator();
  this->RunAction = toolbar->addAction(tr("Run + Load Result"));
  this->ViewMeshAction = toolbar->addAction(tr("View Mesh"));
  toolbar->addSeparator();
  QAction* loadSchemaAction = toolbar->addAction(tr("Load App Schema..."));
  this->RunAction->setEnabled(false);

  connect(newAction, &QAction::triggered, this, &MooseHitEditorPanel::onNewFile);
  connect(openAction, &QAction::triggered, this, &MooseHitEditorPanel::onOpenFile);
  connect(this->SaveAction, &QAction::triggered, this, &MooseHitEditorPanel::onSaveFile);
  connect(saveAsAction, &QAction::triggered, this, &MooseHitEditorPanel::onSaveFileAs);
  connect(this->RunAction, &QAction::triggered, this, &MooseHitEditorPanel::onRunSolve);
  connect(this->ViewMeshAction, &QAction::triggered, this, &MooseHitEditorPanel::onViewMesh);
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
  this->resetToNewRoot();
}

// ---------------------------------------------------------------------------
// File I/O
// ---------------------------------------------------------------------------
void MooseHitEditorPanel::setRootFromParsedText(const QString& fname, const QString& text)
{
  std::vector<hit::ErrorMessage> errors;
  hit::Node* parsed = hit::parse(fname.toStdString(), text.toStdString(), &errors);

  if (this->RootNode)
  {
    delete this->RootNode;
  }
  this->RootNode = parsed;

  if (!errors.empty())
  {
    QString msg;
    for (const auto& e : errors)
    {
      msg += QString::fromStdString(e.prefixed_message) + "\n";
    }
    QMessageBox::warning(this, tr("Parsed with warnings"), msg);
  }
}

void MooseHitEditorPanel::resetToNewRoot()
{
  this->setRootFromParsedText("new_input", "");
  this->CurrentFilePath.clear();
  this->rebuildTree();
  this->markDirty(false);
  this->SaveAction->setEnabled(true);
  this->RunAction->setEnabled(false);
}

void MooseHitEditorPanel::onNewFile()
{
  if (this->Dirty)
  {
    auto reply = QMessageBox::question(
      this, tr("Discard changes?"), tr("The current input has unsaved changes. Start a new one anyway?"));
    if (reply != QMessageBox::Yes)
    {
      return;
    }
  }
  this->resetToNewRoot();
}

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
  QFile file(filePath);
  if (!file.open(QIODevice::ReadOnly | QIODevice::Text))
  {
    QMessageBox::warning(this, tr("Failed to open file"), tr("Could not open %1").arg(filePath));
    return;
  }
  QTextStream in(&file);
  QString text = in.readAll();
  file.close();

  this->setRootFromParsedText(filePath, text);

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
  // render() is hit's own serializer -- it applies MOOSE's real quoting/
  // escaping rules and preserves comments/blank lines that were present
  // in the originally-parsed input and not touched by this editor.
  std::string text = this->RootNode->render();

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
// pipeline.
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
  QString resultPath = inputInfo.absolutePath() + "/" + inputInfo.completeBaseName() + "_out.e";
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
// Find and load the mesh referenced by the [Mesh] block. See
// findFileParamRecursive() above for what this does and doesn't handle.
// ---------------------------------------------------------------------------
QString MooseHitEditorPanel::findMeshFile() const
{
  if (!this->RootNode)
  {
    return QString();
  }
  for (hit::Node* child : this->RootNode->children(hit::NodeType::Section))
  {
    if (child->path() == "Mesh")
    {
      std::string value;
      if (findFileParamRecursive(child, value))
      {
        return QString::fromStdString(value);
      }
      break;
    }
  }
  return QString();
}

QString MooseHitEditorPanel::resolveInputRelativePath(const QString& maybeRelative) const
{
  QFileInfo fi(maybeRelative);
  if (fi.isAbsolute())
  {
    return maybeRelative;
  }
  QFileInfo inputInfo(this->CurrentFilePath);
  return inputInfo.absolutePath() + "/" + maybeRelative;
}

void MooseHitEditorPanel::onViewMesh()
{
  if (this->CurrentFilePath.isEmpty())
  {
    QMessageBox::information(this, tr("Save first"),
      tr("Save the input file before viewing its mesh (the mesh file path is resolved relative to "
         "the input file's location)."));
    return;
  }

  QString meshFile = this->findMeshFile();
  if (meshFile.isEmpty())
  {
    QMessageBox::information(this, tr("No mesh file found"),
      tr("Could not find a `file = ...` parameter under the [Mesh] block or its sub-blocks. "
         "This won't find anything for a generated mesh (e.g. GeneratedMesh) since there's no "
         "file to load -- run the solve and use \"Run + Load Result\" instead to see the mesh "
         "MOOSE actually produced."));
    return;
  }

  QString resolved = this->resolveInputRelativePath(meshFile);
  if (!QFileInfo::exists(resolved))
  {
    QMessageBox::warning(this, tr("Mesh file not found"),
      tr("The [Mesh] block references '%1', which resolved to:\n%2\nbut that file does not exist.")
        .arg(meshFile, resolved));
    return;
  }

  pqObjectBuilder* builder = pqApplicationCore::instance()->getObjectBuilder();
  pqServer* server = pqActiveObjects::instance().activeServer();
  // MOOSE meshes are most commonly Exodus (.e/.exo), which is what this
  // always uses. Other mesh formats MOOSE can read (Gmsh .msh, Abaqus
  // .inp, etc.) would need the reader proxy picked based on file
  // extension instead -- not done here to keep this example focused.
  pqPipelineSource* source =
    builder->createReader("sources", "ExodusIIReader", QStringList(resolved), server);
  if (source)
  {
    source->updatePipeline();
  }
  else
  {
    QMessageBox::warning(
      this, tr("Failed to load mesh"), tr("ParaView's Exodus reader could not open:\n%1").arg(resolved));
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

QString MooseHitEditorPanel::topLevelBlockName(hit::Node* node) const
{
  if (!node)
  {
    return QString();
  }
  while (node->parent() && node->parent()->parent())
  {
    node = node->parent();
  }
  return QString::fromStdString(node->path());
}

// ---------------------------------------------------------------------------
// Tree <-> hit::Node model
// ---------------------------------------------------------------------------
void MooseHitEditorPanel::rebuildTree()
{
  this->Tree->clear();
  if (!this->RootNode)
  {
    return;
  }
  for (hit::Node* child : this->RootNode->children(hit::NodeType::Section))
  {
    this->addTreeItemsRecursive(nullptr, child);
  }
  this->Tree->expandAll();
}

void MooseHitEditorPanel::addTreeItemsRecursive(QTreeWidgetItem* parentItem, hit::Node* node)
{
  QTreeWidgetItem* item = parentItem ? new QTreeWidgetItem(parentItem) : new QTreeWidgetItem(this->Tree);
  item->setText(0, QString::fromStdString(node->path()));
  item->setData(0, Qt::UserRole, QVariant::fromValue(node));

  for (hit::Node* child : node->children(hit::NodeType::Section))
  {
    this->addTreeItemsRecursive(item, child);
  }
}

hit::Node* MooseHitEditorPanel::nodeForItem(QTreeWidgetItem* item) const
{
  if (!item)
  {
    return nullptr;
  }
  return item->data(0, Qt::UserRole).value<hit::Node*>();
}

void MooseHitEditorPanel::onTreeSelectionChanged()
{
  QList<QTreeWidgetItem*> selected = this->Tree->selectedItems();
  hit::Node* node = selected.isEmpty() ? nullptr : this->nodeForItem(selected.first());
  this->populateParamTableFor(node);
}

void MooseHitEditorPanel::populateParamTableFor(hit::Node* node)
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

  for (hit::Node* fieldNode : node->children(hit::NodeType::Field))
  {
    QString paramName = QString::fromStdString(fieldNode->path());
    QString paramValue = QString::fromStdString(fieldNode->strVal());

    int row = this->ParamTable->rowCount();
    this->ParamTable->insertRow(row);

    QTableWidgetItem* nameItem = new QTableWidgetItem(paramName);
    // Tag the row with the actual Field node it represents, so edit
    // handlers don't need to re-derive it from row index or tree
    // selection (which can drift out of sync with table rows).
    nameItem->setData(Qt::UserRole, QVariant::fromValue(fieldNode));

    MooseParamInfo info;
    QStringList options = (paramName == "type") ? typeChoices : QStringList();
    if (this->Schema.paramInfo(paramName, info))
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
      connect(combo, &QComboBox::currentTextChanged, this, [this, fieldNode](const QString& text) {
        static_cast<hit::Field*>(fieldNode)->setVal(text.toStdString());
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
  QTreeWidgetItem* itemAtPos = this->Tree->itemAt(pos);

  QMenu menu(this);
  QAction* addTopLevelAction = menu.addAction(tr("Add Top-Level Block"));
  QAction* addChildAction = itemAtPos ? menu.addAction(tr("Add Child Block")) : nullptr;
  QAction* removeAction = itemAtPos ? menu.addAction(tr("Remove Block")) : nullptr;

  QAction* chosen = menu.exec(this->Tree->viewport()->mapToGlobal(pos));
  if (chosen == addTopLevelAction)
  {
    this->onAddTopLevelBlock();
  }
  else if (addChildAction && chosen == addChildAction)
  {
    this->addBlockUnder(this->nodeForItem(itemAtPos), itemAtPos);
  }
  else if (removeAction && chosen == removeAction)
  {
    this->Tree->setCurrentItem(itemAtPos);
    this->onRemoveBlock();
  }
}

void MooseHitEditorPanel::onAddTopLevelBlock()
{
  // Always parents to the root, regardless of whatever happens to be
  // selected in the tree -- this is the one unambiguous way to add a new
  // [BlockName] at the top level of the input file.
  this->addBlockUnder(this->RootNode, nullptr);
}

void MooseHitEditorPanel::addBlockUnder(hit::Node* parentNode, QTreeWidgetItem* parentItem)
{
  if (!parentNode)
  {
    return;
  }
  bool ok = false;
  QString name = QInputDialog::getText(this, tr("New Block"), tr("Block name:"), QLineEdit::Normal, QString(), &ok);
  if (!ok || name.trimmed().isEmpty())
  {
    return;
  }

  hit::Node* child = new hit::Section(name.trimmed().toStdString());
  parentNode->addChild(child);

  this->addTreeItemsRecursive(parentItem, child);
  if (parentItem)
  {
    parentItem->setExpanded(true);
  }
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
  hit::Node* node = this->nodeForItem(item);
  if (!node || !node->parent())
  {
    return;
  }

  // hit::Node's destructor unlinks itself from its parent's child list
  // (verified against framework/contrib/hit/src/hit/parse.cc), so a plain
  // delete is all that's needed -- no separate "detach" step.
  delete node;
  delete item;
  this->markDirty(true);
}

// ---------------------------------------------------------------------------
// Parameter table <-> hit::Field nodes
// ---------------------------------------------------------------------------
void MooseHitEditorPanel::onParamTableCellChanged(int row, int column)
{
  if (this->BlockTableSignals)
  {
    return;
  }
  QTableWidgetItem* nameItem = this->ParamTable->item(row, NameColumn);
  if (!nameItem)
  {
    return;
  }
  hit::Node* fieldNode = nameItem->data(Qt::UserRole).value<hit::Node*>();
  if (!fieldNode)
  {
    return;
  }

  // setOverridePath renames the field (Field has no dedicated rename
  // setter; this is hit's documented mechanism for it -- it makes every
  // subsequent path()/render() call use the new name).
  fieldNode->setOverridePath(nameItem->text().toStdString());

  // Rows whose value is a schema-backed dropdown have no QTableWidgetItem
  // in the value column (it's a QComboBox instead); those update the
  // field's value themselves via the combo's connected lambda.
  QTableWidgetItem* valueItem = this->ParamTable->item(row, ValueColumn);
  if (valueItem)
  {
    static_cast<hit::Field*>(fieldNode)->setVal(valueItem->text().toStdString());
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
  hit::Node* node = this->nodeForItem(selected.first());
  if (!node)
  {
    return;
  }
  hit::Node* field = new hit::Field("new_param", hit::Field::Kind::String, "value");
  node->addChild(field);
  this->populateParamTableFor(node);
  this->markDirty(true);
}

void MooseHitEditorPanel::onRemoveParamRow()
{
  int row = this->ParamTable->currentRow();
  if (row < 0)
  {
    return;
  }
  QTableWidgetItem* nameItem = this->ParamTable->item(row, NameColumn);
  if (!nameItem)
  {
    return;
  }
  hit::Node* fieldNode = nameItem->data(Qt::UserRole).value<hit::Node*>();
  if (!fieldNode)
  {
    return;
  }
  hit::Node* parentNode = fieldNode->parent();
  delete fieldNode;
  this->populateParamTableFor(parentNode);
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
