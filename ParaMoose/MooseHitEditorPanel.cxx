// MooseHitEditorPanel.cxx
#include "MooseHitEditorPanel.h"

// MOOSE's real hit parser (framework/contrib/hit). See CMakeLists.txt for
// how this gets built/linked (it depends on WASP, a separate compiled
// library that MOOSE builds alongside it).
#include "hit/hit.h"

// ParaView / pqCore
#include "pqActiveObjects.h"
#include "pqApplicationCore.h"
#include "pqDataRepresentation.h"
#include "pqObjectBuilder.h"
#include "pqOutputWidget.h"
#include "pqPipelineSource.h"
#include "pqServer.h"
#include "pqView.h"

#include "vtkSMArraySelectionDomain.h"
#include "vtkSMPVRepresentationProxy.h"
#include "vtkSMProperty.h"
#include "vtkSMPropertyHelper.h"
#include "vtkSMProxy.h"
#include "vtkSMSourceProxy.h"

#include <QAction>
#include <QComboBox>
#include <QFile>
#include <QFileDialog>
#include <QFileInfo>
#include <QFont>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QInputDialog>
#include <QLabel>
#include <QLineEdit>
#include <QMap>
#include <QMenu>
#include <QMessageBox>
#include <QMetaType>
#include <QPoint>
#include <QPlainTextEdit>
#include <QProcess>
#include <QPushButton>
#include <QSplitter>
#include <QStringList>
#include <QTableWidget>
#include <QTabWidget>
#include <QTextStream>
#include <QToolBar>
#include <QTreeWidget>
#include <QVBoxLayout>
#include <QVariant>
#include <QWidget>

#include <algorithm>
#include <exception>
#include <string>
#include <vector>

// Lets QVariant carry raw hit::Node pointers (used to tag tree items and
// parameter-table rows with the tree node they represent). Only used
// within this translation unit, so declaring it here rather than in a
// header is fine.
Q_DECLARE_METATYPE(hit::Node*)

namespace
{
const int NameColumn = 0;
const int ValueColumn = 1;

// Exodus array-selection properties (ElementBlocks, SideSetArrayStatus,
// NodeSetArrayStatus) are NOT a flat list of "enabled" names at the raw
// vtkSMStringVectorProperty level, despite paraview.simple presenting them
// to Python that way. Confirmed against ParaView's own property XML:
//   <StringVectorProperty command="SetSideSetArrayStatus" element_types="2 0"
//     number_of_elements_per_command="2" repeat_command="1" ...>
// number_of_elements_per_command="2" + repeat_command="1" means the real
// element list is interleaved (name, "0"/"1") pairs for every array the
// domain knows about -- not just the ones you want on. Writing only the
// "on" names (an earlier version of this code did exactly that) leaves
// the property in a malformed state that the domain apparently falls back
// from to its default (everything enabled), which is why the whole mesh
// showed up instead of just the named boundary/block.
void setArraySelectionProperty(
  vtkSMSourceProxy* proxy, const char* propertyName, const std::vector<std::string>& wantedNames)
{
  vtkSMProperty* prop = proxy->GetProperty(propertyName);
  if (!prop)
  {
    return;
  }
  vtkSMArraySelectionDomain* domain = vtkSMArraySelectionDomain::SafeDownCast(prop->GetDomain("array_list"));
  if (!domain)
  {
    return;
  }

  unsigned int n = domain->GetNumberOfStrings();
  vtkSMPropertyHelper helper(proxy, propertyName);
  helper.SetNumberOfElements(n * 2);
  for (unsigned int i = 0; i < n; ++i)
  {
    std::string name = domain->GetString(i);
    bool wanted = std::find(wantedNames.begin(), wantedNames.end(), name) != wantedNames.end();
    helper.Set(2 * i, name.c_str());
    helper.Set(2 * i + 1, wanted ? "1" : "0");
  }
}

// Maps a mesh file's extension to the ParaView reader proxy that can open
// it. Deliberately conservative: only extensions backed by a proxy name
// confirmed against ParaView's own documentation/source are included here
// (Exodus family, legacy VTK, VTK XML unstructured grid). MOOSE/libMesh
// can also read several other formats (Gmsh .msh, Abaqus .inp, ANSYS .cdb,
// Nastran, etc.) that aren't wired up -- guessing at proxy names for those
// risks a confusing runtime failure instead of the clear "unsupported"
// message this returns for them.
QString readerProxyNameForExtension(const QString& filePath)
{
  QString ext = QFileInfo(filePath).suffix().toLower();
  static const QMap<QString, QString> kExtensionToProxy = {
    { "e", "ExodusIIReader" },
    { "exo", "ExodusIIReader" },
    { "ex2", "ExodusIIReader" },
    { "exii", "ExodusIIReader" },
    { "gen", "ExodusIIReader" }, // Exodus/Genesis, same reader
    { "g", "ExodusIIReader" },
    { "nem", "ExodusIIReader" }, // Nemesis (parallel Exodus), same reader
    { "vtk", "LegacyVTKFileReader" },
    { "vtu", "XMLUnstructuredGridReader" },
  };
  return kExtensionToProxy.value(ext);
}

// Only Exodus-family files carry the named element-block/sideset/nodeset
// structure the highlighting feature (see setArraySelectionProperty()
// below) depends on -- legacy VTK / VTK XML files have no equivalent
// concept at the reader level.
bool isExodusFamilyExtension(const QString& filePath)
{
  return readerProxyNameForExtension(filePath) == "ExodusIIReader";
}

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
  if (this->SolveProcess)
  {
    this->SolveProcess->kill();
    this->SolveProcess->waitForFinished(3000);
  }
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
#ifdef MOOSE_DEFAULT_APP_EXECUTABLE
  // Set via -DMOOSE_DIR=... at configure time (see CMakeLists.txt); just a
  // starting point, still fully editable.
  this->ExecutablePathEdit->setText(QStringLiteral(MOOSE_DEFAULT_APP_EXECUTABLE));
#endif
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
  connect(this->ParamTable, &QTableWidget::currentCellChanged, this,
    &MooseHitEditorPanel::onParamTableCurrentCellChanged);
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

  // --- Raw text tab -----------------------------------------------
  // An editable preview of exactly what render() would write to disk.
  // Deliberately NOT kept live in sync with the structured view on every
  // keystroke in either direction -- see onMainTabChanged()/onApplyRawText()
  // /onRefreshRawTextFromModel() for how the two stay in sync without
  // either one silently clobbering unapplied edits in the other.
  QWidget* rawTextPage = new QWidget();
  QVBoxLayout* rawTextLayout = new QVBoxLayout(rawTextPage);
  rawTextLayout->setContentsMargins(0, 0, 0, 0);

  QToolBar* rawTextToolbar = new QToolBar(rawTextPage);
  QAction* applyRawAction = rawTextToolbar->addAction(tr("Apply Changes"));
  QAction* refreshRawAction = rawTextToolbar->addAction(tr("Refresh from Model"));
  connect(applyRawAction, &QAction::triggered, this, &MooseHitEditorPanel::onApplyRawText);
  connect(refreshRawAction, &QAction::triggered, this, &MooseHitEditorPanel::onRefreshRawTextFromModel);
  rawTextLayout->addWidget(rawTextToolbar);

  this->RawTextEdit = new QPlainTextEdit(rawTextPage);
  this->RawTextEdit->setLineWrapMode(QPlainTextEdit::NoWrap);
  QFont rawTextFont("Monospace");
  rawTextFont.setStyleHint(QFont::TypeWriter);
  this->RawTextEdit->setFont(rawTextFont);
  this->RawTextEdit->setStyleSheet(
    "QPlainTextEdit { background-color: #000000; color: #ffffff; "
    "selection-background-color: #444444; }");
  rawTextLayout->addWidget(this->RawTextEdit, 1);

  this->MainTabs = new QTabWidget(container);
  this->MainTabs->addTab(splitter, tr("Structured"));
  this->RawTextTabIndex = this->MainTabs->addTab(rawTextPage, tr("Raw Text"));
  connect(this->MainTabs, &QTabWidget::currentChanged, this, &MooseHitEditorPanel::onMainTabChanged);

  QSplitter* verticalSplitter = new QSplitter(Qt::Vertical, container);
  verticalSplitter->addWidget(this->MainTabs);

  this->OutputWidget = new pqOutputWidget(verticalSplitter);
  this->OutputWidget->setSettingsKey("ParaMooseSolverOutput");
  verticalSplitter->addWidget(this->OutputWidget);
  verticalSplitter->setStretchFactor(0, 3);
  verticalSplitter->setStretchFactor(1, 1);

  mainLayout->addWidget(verticalSplitter, 1);

  this->setWidget(container);
  this->resetToNewRoot();
}

// ---------------------------------------------------------------------------
// File I/O
// ---------------------------------------------------------------------------
void MooseHitEditorPanel::setRootFromParsedText(const QString& fname, const QString& text)
{
  std::vector<hit::ErrorMessage> errors;
  hit::Node* parsed = nullptr;
  QString exceptionMessage;
  try
  {
    // Documented behavior is that syntax_errors being non-null means parse
    // failures get reported through *errors* rather than thrown -- but
    // that guarantee turned out not to cover every edge case (an entirely
    // empty document being the one that actually bit this plugin; see
    // resetToNewRoot()). Wrapping this in try/catch is a safety net so a
    // parser-level exception on some other edge case can't take down the
    // whole ParaView process the way an uncaught one did here.
    parsed = hit::parse(fname.toStdString(), text.toStdString(), &errors);
  }
  catch (const std::exception& e)
  {
    exceptionMessage = QString::fromStdString(e.what());
  }
  catch (...)
  {
    exceptionMessage = tr("(no exception details available)");
  }

  if (!parsed)
  {
    QMessageBox::warning(this, tr("Failed to parse"),
      exceptionMessage.isEmpty()
        ? tr("hit::parse() returned no data for this input.")
        : tr("hit::parse() threw an exception: %1").arg(exceptionMessage));
    // Leave the existing RootNode (if any) untouched rather than replacing
    // it with nothing -- whatever was open/being edited is still intact.
    return;
  }

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

  // Every path that replaces RootNode (New, Open, and Apply Changes on the
  // Raw Text tab) funnels through here, so this is the one place that
  // needs to re-sync the raw text preview to match.
  this->refreshRawTextFromModel();
}

// Tears down RootNode without ever calling hit::parse()/render() on empty
// content. Rendering (or possibly parsing -- the exact failure point
// wasn't pinned down, see the "Known limitations" note in the README)
// appears to crash outright for a document with zero top-level blocks in
// it, rather than reporting a clean error. Rather than keep chasing
// exactly which call is unsafe on trivial input, RootNode is now simply
// left null whenever there's no real content, and the first
// "Add Top-Level Block" bootstraps a real root together with that first
// block in a single hit::parse() call that's never trivial/empty (see
// addBlockUnder()).
void MooseHitEditorPanel::clearRootNodeWithoutParsing()
{
  if (this->RootNode)
  {
    delete this->RootNode;
    this->RootNode = nullptr;
  }
  this->rebuildTree();
  this->refreshRawTextFromModel();
}

void MooseHitEditorPanel::resetToNewRoot()
{
  this->clearRootNodeWithoutParsing();
  this->CurrentFilePath.clear();
  this->markDirty(false);
  this->SaveAction->setEnabled(true);
  this->RunAction->setEnabled(false);
}

// ---------------------------------------------------------------------------
// Raw text tab: an editable render() preview. The two views (structured
// tree/table vs. raw text) are kept in sync explicitly rather than live,
// on the theory that silently overwriting whichever one the person didn't
// just touch is worse than asking. Three sync points:
//   - refreshRawTextFromModel(): raw text <- model (always available via
//     the "Refresh from Model" button; also called automatically by
//     setRootFromParsedText() whenever RootNode is replaced).
//   - onApplyRawText(): model <- raw text (re-parses the edited text
//     through the same hit::parse() path as opening a file).
//   - onMainTabChanged(): auto-refreshes raw text from the model when
//     switching TO that tab, but only if there are no unapplied edits
//     already sitting there (compared against LastSyncedRawText) --
//     switching tabs back and forth never destroys in-progress typing.
// ---------------------------------------------------------------------------
void MooseHitEditorPanel::refreshRawTextFromModel()
{
  if (!this->RawTextEdit)
  {
    // Called once already during the very first setRootFromParsedText()
    // in buildUi(), by which point RawTextEdit exists -- this guard is
    // just defensive in case that ordering ever changes.
    return;
  }
  QString text = this->RootNode ? QString::fromStdString(this->RootNode->render()) : QString();
  this->RawTextEdit->setPlainText(text);
  this->LastSyncedRawText = text;
}

void MooseHitEditorPanel::onMainTabChanged(int index)
{
  if (index != this->RawTextTabIndex)
  {
    return;
  }
  if (this->RawTextEdit->toPlainText() == this->LastSyncedRawText)
  {
    // Nothing unapplied sitting in the editor -- safe to pull in whatever
    // changed on the structured side since we last showed this tab.
    this->refreshRawTextFromModel();
  }
}

void MooseHitEditorPanel::onRefreshRawTextFromModel()
{
  if (this->RawTextEdit->toPlainText() != this->LastSyncedRawText)
  {
    auto reply = QMessageBox::question(this, tr("Discard raw text edits?"),
      tr("The raw text has changes that haven't been applied. Refreshing from the model will "
         "discard them. Continue?"));
    if (reply != QMessageBox::Yes)
    {
      return;
    }
  }
  this->refreshRawTextFromModel();
}

void MooseHitEditorPanel::onApplyRawText()
{
  QString text = this->RawTextEdit->toPlainText();
  this->ParamTable->setRowCount(0);

  if (text.trimmed().isEmpty())
  {
    // Same crash surface as an empty "New" document -- see the comment on
    // clearRootNodeWithoutParsing() for why this avoids hit::parse()/
    // render() entirely here rather than calling them on blank text.
    this->clearRootNodeWithoutParsing();
    this->markDirty(true);
    return;
  }

  // setRootFromParsedText() replaces RootNode wholesale, which invalidates
  // every hit::Node* the tree/table currently hold onto -- the param table
  // was already cleared above; rebuildTree() below handles the tree
  // itself. It also re-syncs RawTextEdit afterward (see its own comment),
  // so what's shown after Apply is the canonical re-rendered form of what
  // was just typed, which doubles as a way to confirm what was actually
  // parsed.
  QString fname = this->CurrentFilePath.isEmpty() ? QStringLiteral("raw_text_edit") : this->CurrentFilePath;
  this->setRootFromParsedText(fname, text);
  this->rebuildTree();
  this->markDirty(true);
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

  if (text.trimmed().isEmpty())
  {
    // Same crash surface as an empty "New" document -- see the comment on
    // clearRootNodeWithoutParsing().
    this->clearRootNodeWithoutParsing();
  }
  else
  {
    this->setRootFromParsedText(filePath, text);
    this->rebuildTree();
  }

  this->CurrentFilePath = filePath;
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
    QMessageBox::information(
      this, tr("Nothing to save"), tr("Add at least one top-level block before saving."));
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
  if (this->SolveProcess)
  {
    QMessageBox::information(this, tr("Already running"), tr("A solve is already in progress."));
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

  this->OutputWidget->clear();
  this->OutputWidget->displayMessage(tr("Running: %1 -i %2").arg(exe, inputInfo.fileName()));
  this->RunAction->setEnabled(false);

  // Parented to `this` for Qt's ownership tree as a fallback, but the
  // finished()/errorOccurred() handlers below deleteLater() it explicitly
  // and null out SolveProcess as soon as the run is over, since that null
  // check is also how the rest of the panel knows whether a solve is
  // currently in flight.
  this->SolveProcess = new QProcess(this);
  this->SolveProcess->setWorkingDirectory(inputInfo.absolutePath());
  this->SolveProcess->setProgram(exe);
  this->SolveProcess->setArguments(QStringList() << "-i" << inputInfo.fileName());
  this->SolveProcess->setProcessChannelMode(QProcess::MergedChannels);

  connect(this->SolveProcess, &QProcess::readyReadStandardOutput, this, [this]() {
    if (!this->SolveProcess)
    {
      return;
    }
    QString chunk = QString::fromLocal8Bit(this->SolveProcess->readAllStandardOutput());
    for (const QString& line : chunk.split('\n', Qt::SkipEmptyParts))
    {
      this->OutputWidget->displayMessage(line);
    }
  });

  connect(this->SolveProcess, &QProcess::errorOccurred, this, [this](QProcess::ProcessError) {
    this->OutputWidget->displayMessage(tr("Failed to start solver process."), QtCriticalMsg);
    this->RunAction->setEnabled(true);
    if (this->SolveProcess)
    {
      this->SolveProcess->deleteLater();
      this->SolveProcess = nullptr;
    }
  });

  connect(this->SolveProcess,
    static_cast<void (QProcess::*)(int, QProcess::ExitStatus)>(&QProcess::finished), this,
    [this, inputInfo](int exitCode, QProcess::ExitStatus exitStatus) {
      this->RunAction->setEnabled(true);

      if (exitStatus != QProcess::NormalExit || exitCode != 0)
      {
        this->OutputWidget->displayMessage(
          tr("Solve failed (exit code %1). See output above.").arg(exitCode), QtCriticalMsg);
        if (this->SolveProcess)
        {
          this->SolveProcess->deleteLater();
          this->SolveProcess = nullptr;
        }
        return;
      }
      this->OutputWidget->displayMessage(tr("Solve finished successfully."));

      // MOOSE's default output naming is "<input-file-stem>_out.e".
      QString resultPath = inputInfo.absolutePath() + "/" + inputInfo.completeBaseName() + "_out.e";
      if (!QFileInfo::exists(resultPath))
      {
        this->OutputWidget->displayMessage(
          tr("Expected result file not found: %1").arg(resultPath), QtWarningMsg);
      }
      else
      {
        pqObjectBuilder* builder = pqApplicationCore::instance()->getObjectBuilder();
        pqServer* server = pqActiveObjects::instance().activeServer();
        pqPipelineSource* source =
          builder->createReader("sources", "ExodusIIReader", QStringList(resultPath), server);
        if (source)
        {
          source->updatePipeline();
          this->LastLoadedMeshFilePath = resultPath;
        }
      }

      if (this->SolveProcess)
      {
        this->SolveProcess->deleteLater();
        this->SolveProcess = nullptr;
      }
    });

  this->SolveProcess->start();
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

  QString readerProxyName = readerProxyNameForExtension(resolved);
  if (readerProxyName.isEmpty())
  {
    QMessageBox::warning(this, tr("Unsupported mesh format"),
      tr("Don't know how to open '%1' -- only Exodus (.e/.exo/.ex2/.gen/.g/.nem), legacy VTK "
         "(.vtk), and VTK XML unstructured grid (.vtu) are wired up. MOOSE/libMesh can read "
         "several other formats (Gmsh, Abaqus, ANSYS, Nastran, ...) that this plugin doesn't "
         "have a reader mapping for yet.")
        .arg(QFileInfo(resolved).fileName()));
    return;
  }

  pqObjectBuilder* builder = pqApplicationCore::instance()->getObjectBuilder();
  pqServer* server = pqActiveObjects::instance().activeServer();
  pqView* view = pqActiveObjects::instance().activeView();
  pqPipelineSource* source =
    builder->createReader("sources", readerProxyName, QStringList(resolved), server);
  if (source)
  {
    source->updatePipeline();
    this->LastLoadedMeshFilePath = resolved;

    // Enable visibility of the mesh as a wireframe representation in the active view. This is the same as what
    pqDataRepresentation* sourceRepresentation = builder->createDataRepresentation(source->getOutputPort(0), view);
    vtkSMProxy* reprProxy = sourceRepresentation->getProxy();
    vtkSMPropertyHelper(reprProxy, "Representation").Set("Wireframe");
    reprProxy->UpdateVTKObjects();
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
  bool ok = false;
  QString name = QInputDialog::getText(this, tr("New Block"), tr("Block name:"), QLineEdit::Normal, QString(), &ok);
  if (!ok || name.trimmed().isEmpty())
  {
    return;
  }
  QString trimmedName = name.trimmed();

  if (!parentNode)
  {
    // parentNode is only ever null here for the "Add Top-Level Block"
    // case when there's no root yet (a brand-new, never-saved input --
    // see resetToNewRoot()). Bootstrap the root and this first top-level
    // section together in one hit::parse() call with real content in it,
    // rather than ever parsing/rendering a genuinely empty document (see
    // clearRootNodeWithoutParsing() for why that's avoided entirely now).
    QString fname = this->CurrentFilePath.isEmpty() ? QStringLiteral("new_input") : this->CurrentFilePath;
    this->setRootFromParsedText(fname, "[" + trimmedName + "]\n[]\n");
    this->rebuildTree();
    this->markDirty(true);
    return;
  }

  hit::Node* child = new hit::Section(trimmedName.toStdString());
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

// ---------------------------------------------------------------------------
// Highlighting: when the current table row is a "boundary" or "block"
// parameter, show the mesh region(s) it names as a colored overlay in the
// render view. See the class-level comment on HighlightSource for the
// overall approach (a second, hidden reader + representation, kept
// separate from whatever the person is actually looking at).
// ---------------------------------------------------------------------------
void MooseHitEditorPanel::onParamTableCurrentCellChanged(int currentRow, int, int, int)
{
  if (currentRow < 0)
  {
    this->clearHighlight();
    return;
  }
  QTableWidgetItem* nameItem = this->ParamTable->item(currentRow, NameColumn);
  if (!nameItem)
  {
    this->clearHighlight();
    return;
  }
  hit::Node* fieldNode = nameItem->data(Qt::UserRole).value<hit::Node*>();
  QString paramName = nameItem->text();
  if (!fieldNode || (paramName != "boundary" && paramName != "block"))
  {
    this->clearHighlight();
    return;
  }
  this->updateHighlight(fieldNode, paramName);
}

void MooseHitEditorPanel::ensureHighlightSource(const QString& meshFilePath)
{
  if (this->HighlightSource && this->HighlightSourceFilePath == meshFilePath)
  {
    return;
  }

  pqObjectBuilder* builder = pqApplicationCore::instance()->getObjectBuilder();
  if (this->HighlightSource)
  {
    builder->destroy(this->HighlightSource);
    this->HighlightSource = nullptr;
    this->HighlightRepresentation = nullptr;
  }

  if (!isExodusFamilyExtension(meshFilePath))
  {
    // Highlighting depends on Exodus's named sideset/nodeset/element-block
    // structure via SideSetArrayStatus etc.; legacy VTK / VTK XML files
    // have no equivalent, so there's nothing useful to build here. Leave
    // HighlightSource null -- updateHighlight() already no-ops on that.
    return;
  }

  pqServer* server = pqActiveObjects::instance().activeServer();
  pqView* view = pqActiveObjects::instance().activeView();
  this->HighlightSource =
    builder->createReader("sources", "ExodusIIReader", QStringList(meshFilePath), server);
  if (!this->HighlightSource)
  {
    return;
  }
  this->HighlightSourceFilePath = meshFilePath;

  // Domains for the array-selection properties below (i.e. the list of
  // sideset/nodeset/element-block names actually available) aren't
  // populated until the reader has read the file's metadata at least
  // once -- do that before touching those properties.
  this->HighlightSource->updatePipeline();

  // Start with everything hidden from this second reader's own output --
  // updateHighlight() below turns on just the specific sideset/nodeset/
  // element-block names for whatever's currently selected.
  vtkSMSourceProxy* proxy = vtkSMSourceProxy::SafeDownCast(this->HighlightSource->getProxy());
  if (proxy)
  {
    setArraySelectionProperty(proxy, "ElementBlocks", {});
    setArraySelectionProperty(proxy, "SideSetArrayStatus", {});
    setArraySelectionProperty(proxy, "NodeSetArrayStatus", {});
    proxy->UpdateVTKObjects();
  }

  this->HighlightRepresentation = builder->createDataRepresentation(this->HighlightSource->getOutputPort(0), view);
  if (this->HighlightRepresentation)
  {
    vtkSMProxy* reprProxy = this->HighlightRepresentation->getProxy();
    // Solid red, not mapped from any data array. SetScalarColoring is an
    // instance method on vtkSMPVRepresentationProxy (not static, despite
    // looking like a natural fit for one) -- confirmed against ParaView's
    // own header before using it, since guessing wrong here would have
    // silently left the highlight scalar-colored instead of solid.
    // arrayName == nullptr turns coloring off per that header's own doc
    // comment; the attribute-type argument is a vtkDataObject::AttributeTypes
    // value and is irrelevant once arrayName is null, so 0 (POINT) is fine.
    static const double kHighlightColor[3] = { 1.0, 0.0, 0.0 };
    if (vtkSMPVRepresentationProxy* pvRepr = vtkSMPVRepresentationProxy::SafeDownCast(reprProxy))
    {
      pvRepr->SetScalarColoring(nullptr, 0);
    }
    vtkSMPropertyHelper(reprProxy, "DiffuseColor").Set(kHighlightColor, 3);
    vtkSMPropertyHelper(reprProxy, "AmbientColor").Set(kHighlightColor, 3);
    reprProxy->UpdateVTKObjects();
    this->HighlightRepresentation->setVisible(false);
  }
}

void MooseHitEditorPanel::updateHighlight(hit::Node* fieldNode, const QString& paramName)
{
  if (this->LastLoadedMeshFilePath.isEmpty())
  {
    // Nothing loaded yet (no "View Mesh" / "Run + Load Result" this
    // session) -- nothing to highlight against.
    return;
  }
  this->ensureHighlightSource(this->LastLoadedMeshFilePath);
  if (!this->HighlightSource || !this->HighlightRepresentation)
  {
    return;
  }

  QString value = QString::fromStdString(static_cast<hit::Field*>(fieldNode)->strVal());
  QStringList tokens = value.split(' ', Qt::SkipEmptyParts);
  std::vector<std::string> names;
  for (const QString& t : tokens)
  {
    names.push_back(t.toStdString());
  }

  vtkSMSourceProxy* proxy = vtkSMSourceProxy::SafeDownCast(this->HighlightSource->getProxy());
  if (!proxy)
  {
    return;
  }

  // "boundary" in MOOSE can name either a sideset or a nodeset (surface vs
  // nodal BCs both use the same parameter name), so try both -- whichever
  // doesn't match anything in this mesh simply shows nothing extra.
  // "block" always means element blocks.
  if (paramName == "boundary")
  {
    setArraySelectionProperty(proxy, "SideSetArrayStatus", names);
    setArraySelectionProperty(proxy, "NodeSetArrayStatus", names);
    setArraySelectionProperty(proxy, "ElementBlocks", {});
  }
  else
  {
    setArraySelectionProperty(proxy, "ElementBlocks", names);
    setArraySelectionProperty(proxy, "SideSetArrayStatus", {});
    setArraySelectionProperty(proxy, "NodeSetArrayStatus", {});
  }
  proxy->UpdateVTKObjects();
  this->HighlightSource->updatePipeline();
  this->HighlightRepresentation->setVisible(true);

  if (pqView* view = pqActiveObjects::instance().activeView())
  {
    view->render();
  }
}

void MooseHitEditorPanel::clearHighlight()
{
  if (this->HighlightRepresentation && this->HighlightRepresentation->isVisible())
  {
    this->HighlightRepresentation->setVisible(false);
    if (pqView* view = pqActiveObjects::instance().activeView())
    {
      view->render();
    }
  }
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
