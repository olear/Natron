//  Natron
//
/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */
/*
 * Created by Alexandre GAUTHIER-FOICHAT on 6/1/2012.
 * contact: immarespond at gmail dot com
 *
 */

#include "MultiInstancePanel.h"

CLANG_DIAG_OFF(deprecated)
CLANG_DIAG_OFF(uninitialized)
#include <QVBoxLayout>
#include <QPixmap>
#include <QDebug>
#include <QCoreApplication>
#include <QThread>
#include <QHeaderView>
#include <QStyledItemDelegate>
#include <QUndoCommand>
#include <QPainter>
#include <QLabel>
#include <QWaitCondition>
#include <QtConcurrentMap>
#include <QMenu>
CLANG_DIAG_ON(deprecated)
CLANG_DIAG_ON(uninitialized)

#include <boost/bind.hpp>

#include "Gui/Button.h"
#include "Gui/ComboBox.h"
#include "Gui/GuiApplicationManager.h"
#include "Gui/GuiAppInstance.h"
#include "Gui/AnimatedCheckBox.h"
#include "Gui/SpinBox.h"
#include "Gui/TableModelView.h"
#include "Gui/NodeGui.h"
#include "Gui/DockablePanel.h"
#include "Gui/NodeGraph.h"
#include "Gui/Gui.h"

#include "Engine/Node.h"
#include "Engine/KnobTypes.h"
#include "Engine/KnobFile.h"
#include "Engine/EffectInstance.h"
#include "Engine/Curve.h"
#include "Engine/TimeLine.h"

#include <ofxNatron.h>

#define kTrackBackwardButtonName "trackBackward"
#define kTrackPreviousButtonName "trackPrevious"
#define kTrackNextButtonName "trackNext"
#define kTrackForwardButtonName "trackForward"
#define kTrackCenterName "center"
#define kTrackInvertName "invert"
using namespace Natron;

namespace {
typedef std::list < std::pair<boost::shared_ptr<Node>,bool> > Nodes;

boost::shared_ptr<Double_Knob>
getCenterKnobForTracker(Node* node)
{
    boost::shared_ptr<KnobI> knob = node->getKnobByName(kTrackCenterName);

    assert(knob);
    boost::shared_ptr<Double_Knob> dblKnob = boost::dynamic_pointer_cast<Double_Knob>(knob);
    assert(dblKnob);

    return dblKnob;
}
}

struct MultiInstancePanelPrivate
{
    MultiInstancePanel* publicInterface;
    bool guiCreated;
    boost::shared_ptr<NodeGui> mainInstance;
    //pair <pointer,selected?>
    Nodes instances;
    TableView* view;
    TableModel* model;
    QWidget* buttonsContainer;
    QHBoxLayout* buttonsLayout;
    Button* addButton;
    Button* removeButton;
    Button* selectAll;
    Button* resetTracksButton;

    ///Set to true when we receive a signal from a knob value change
    ///this is to avoid infinite recursion with the dataChanged signal from the TableItem
    bool executingKnobValueChanged;

    ///same as above but when we're dealing with unslave/slaving parameters
    int knobValueRecursion;

    MultiInstancePanelPrivate(MultiInstancePanel* publicI,
                              const boost::shared_ptr<NodeGui> & node)
        : publicInterface(publicI)
          , guiCreated(false)
          , mainInstance(node)
          , instances()
          , view(0)
          , model(0)
          , buttonsContainer(0)
          , buttonsLayout(0)
          , addButton(0)
          , removeButton(0)
          , selectAll(0)
          , resetTracksButton(0)
          , executingKnobValueChanged(false)
          , knobValueRecursion(0)
    {
    }

    boost::shared_ptr<Natron::Node> getMainInstance() const
    {
        return mainInstance->getNode();
    }

    /**
     * @brief Called to make an exact copy of a main-instance's knob. The resulting copy will
     * be what is displayed on the GUI
     **/
    void createKnob(const boost::shared_ptr<KnobI> & ref)
    {
        if ( ref->isInstanceSpecific() ) {
            return;
        }

        bool declaredByPlugin = ref->isDeclaredByPlugin();
        Button_Knob* isButton = dynamic_cast<Button_Knob*>( ref.get() );
        Choice_Knob* isChoice = dynamic_cast<Choice_Knob*>( ref.get() );
        String_Knob* isString = dynamic_cast<String_Knob*>( ref.get() );
        Double_Knob* isDouble = dynamic_cast<Double_Knob*>( ref.get() );
        Int_Knob* isInt = dynamic_cast<Int_Knob*>( ref.get() );
        
        boost::shared_ptr<KnobHelper> ret;
        if ( isInt  ) {
            boost::shared_ptr<Int_Knob> intKnb = Natron::createKnob<Int_Knob>(publicInterface, ref->getDescription(),ref->getDimension(),declaredByPlugin);
            intKnb->setMinimumsAndMaximums(isInt->getMinimums(), isInt->getMaximums());
            intKnb->setDisplayMinimumsAndMaximums(isInt->getDisplayMinimums(), isInt->getDisplayMaximums());
            ret = intKnb;
        } else if ( dynamic_cast<Bool_Knob*>( ref.get() ) ) {
            ret = Natron::createKnob<Bool_Knob>(publicInterface, ref->getDescription(),ref->getDimension(),declaredByPlugin);
        } else if ( isDouble ) {
            boost::shared_ptr<Double_Knob> dblKnob = Natron::createKnob<Double_Knob>(publicInterface, ref->getDescription(),ref->getDimension(),declaredByPlugin);
            dblKnob->setMinimumsAndMaximums(isDouble->getMinimums(), isDouble->getMaximums());
            dblKnob->setDisplayMinimumsAndMaximums(isDouble->getDisplayMinimums(), isDouble->getDisplayMaximums());
            ret = dblKnob;
        } else if (isChoice) {
            boost::shared_ptr<Choice_Knob> choice = Natron::createKnob<Choice_Knob>(publicInterface,
                                                                                    ref->getDescription(),ref->getDimension(),declaredByPlugin);
            choice->populateChoices( isChoice->getEntries_mt_safe(),isChoice->getEntriesHelp_mt_safe() );
            ret = choice;
        } else if (isString) {
            boost::shared_ptr<String_Knob> strKnob = Natron::createKnob<String_Knob>(publicInterface,
                                                                                     ref->getDescription(),ref->getDimension(),declaredByPlugin);
            if ( isString->isCustomKnob() ) {
                strKnob->setAsCustom();
            }
            if ( isString->isMultiLine() ) {
                strKnob->setAsMultiLine();
            }
            if ( isString->isLabel() ) {
                strKnob->setAsLabel();
            }
            if ( isString->usesRichText() ) {
                strKnob->setUsesRichText(true);
            }
            ret = strKnob;
        } else if ( dynamic_cast<Parametric_Knob*>( ref.get() ) ) {
            ret = Natron::createKnob<Parametric_Knob>(publicInterface, ref->getDescription(),ref->getDimension(),declaredByPlugin);
        } else if ( dynamic_cast<Color_Knob*>( ref.get() ) ) {
            ret = Natron::createKnob<Color_Knob>(publicInterface, ref->getDescription(),ref->getDimension(),declaredByPlugin);
        } else if ( dynamic_cast<Path_Knob*>( ref.get() ) ) {
            ret = Natron::createKnob<Path_Knob>(publicInterface, ref->getDescription(),ref->getDimension(),declaredByPlugin);
        } else if ( dynamic_cast<File_Knob*>( ref.get() ) ) {
            ret = Natron::createKnob<File_Knob>(publicInterface, ref->getDescription(),ref->getDimension(),declaredByPlugin);
        } else if ( dynamic_cast<OutputFile_Knob*>( ref.get() ) ) {
            ret = Natron::createKnob<OutputFile_Knob>(publicInterface, ref->getDescription(),ref->getDimension(),declaredByPlugin);
        } else if (isButton) {
            boost::shared_ptr<Button_Knob> btn = Natron::createKnob<Button_Knob>(publicInterface,
                                                                                 ref->getDescription(),ref->getDimension(),declaredByPlugin);
            ///set the name prior to calling setIconForButton
            btn->setName( ref->getName() );
            publicInterface->setIconForButton( btn.get() );
            ret = btn;
        } else if ( dynamic_cast<Page_Knob*>( ref.get() ) ) {
            ret = Natron::createKnob<Page_Knob>(publicInterface, ref->getDescription(),ref->getDimension(),declaredByPlugin);
        }
        assert(ret);
        ret->clone(ref);
        ret->setName( ref->getName() );
        ret->setAnimationEnabled( ref->isAnimationEnabled() );
        ret->setHintToolTip( ref->getHintToolTip() );
        ret->setEvaluateOnChange( ref->getEvaluateOnChange() );
        ret->setIsPersistant(false);
        if ( ref->isNewLineTurnedOff() ) {
            ret->turnOffNewLine();
        }
        bool refSecret = ref->getIsSecret();

        if (refSecret) {
            ret->setSecret(true);
        }
    } // createKnob

    void addTableRow(const boost::shared_ptr<Natron::Node> & node);

    void removeRow(int index);

    void getInstanceSpecificKnobs(const Node* node,
                                  std::list<boost::shared_ptr<KnobI> >* knobs) const
    {
        const std::vector<boost::shared_ptr<KnobI> > & instanceKnobs = node->getKnobs();

        for (U32 i = 0; i < instanceKnobs.size(); ++i) {
            Int_Knob* isInt = dynamic_cast<Int_Knob*>( instanceKnobs[i].get() );
            Bool_Knob* isBool = dynamic_cast<Bool_Knob*>( instanceKnobs[i].get() );
            Double_Knob* isDouble = dynamic_cast<Double_Knob*>( instanceKnobs[i].get() );
            Color_Knob* isColor = dynamic_cast<Color_Knob*>( instanceKnobs[i].get() );
            String_Knob* isString = dynamic_cast<String_Knob*>( instanceKnobs[i].get() );

            if ( instanceKnobs[i]->isInstanceSpecific() ) {
                if (!isInt && !isBool && !isDouble && !isColor && !isString) {
                    qDebug() << "Multi-instance panel doesn't support the following type of knob: " << instanceKnobs[i]->typeName().c_str();
                    continue;
                }

                knobs->push_back(instanceKnobs[i]);
            }
        }
    }

    void getNodesFromSelection(const QModelIndexList & indexes,std::list<std::pair<Node*,bool> >* nodes);

    void pushUndoCommand(QUndoCommand* cmd)
    {
        mainInstance->getSettingPanel()->pushUndoCommand(cmd);
    }

    boost::shared_ptr<Natron::Node> getInstanceFromItem(TableItem* item) const;
};

MultiInstancePanel::MultiInstancePanel(const boost::shared_ptr<NodeGui> & node)
    : NamedKnobHolder( node->getNode()->getApp() )
      , _imp( new MultiInstancePanelPrivate(this,node) )
{
}

MultiInstancePanel::~MultiInstancePanel()
{
}

////////////// TableView delegate

class TableItemDelegate
    : public QStyledItemDelegate
{
    TableView* _view;
    MultiInstancePanel* _panel;

public:

    explicit TableItemDelegate(TableView* view,
                               MultiInstancePanel* panel);

private:

    virtual void paint(QPainter * painter, const QStyleOptionViewItem & option, const QModelIndex & index) const OVERRIDE FINAL;
};

TableItemDelegate::TableItemDelegate(TableView* view,
                                     MultiInstancePanel* panel)
    : QStyledItemDelegate(view)
      , _view(view)
      , _panel(panel)
{
}

void
TableItemDelegate::paint(QPainter * painter,
                         const QStyleOptionViewItem & option,
                         const QModelIndex & index) const
{
    QStyledItemDelegate::paint(painter,option,index);

    if (!index.isValid() || (index.column() == 0) || option.state & QStyle::State_Selected) {
        QStyledItemDelegate::paint(painter,option,index);

        return;
    }
    TableModel* model = dynamic_cast<TableModel*>( _view->model() );
    assert(model);
    if (!model) {
        // coverity[dead_error_line]
        return;
    }
    TableItem* item = model->item(index);
    assert(item);
    if (!item) {
        // coverity[dead_error_line]
        return;
    }
    int dim;
    boost::shared_ptr<KnobI> knob = _panel->getKnobForItem(item, &dim);
    assert(knob);
    if (!knob) {
        // coverity[dead_error_line]
        return;
    }
    assert(0 <= dim);
    Natron::AnimationLevelEnum level = knob->getAnimationLevel(dim);
    if (level == eAnimationLevelNone) {
        QStyledItemDelegate::paint(painter,option,index);

        return;
    }

    QStyleOptionViewItem opt = option;
    initStyleOption(&opt, index);

    const QWidget* widget = _view->cellWidget( index.row(), index.column() );
    if (!widget) {
        QStyledItemDelegate::paint(painter,option,index);

        return;
    }
    QColor bgColor;
    if (level == eAnimationLevelOnKeyframe) {
        bgColor.setRgb(21,97,248);
    } else if (level == eAnimationLevelInterpolatedValue) {
        bgColor.setRgb(86,117,156);
    }


    //   widget->render(painter);
}

boost::shared_ptr<Natron::Node> MultiInstancePanel::getMainInstance() const
{
    return _imp->getMainInstance();
}

Gui*
MultiInstancePanel::getGui() const
{
    return _imp->mainInstance->getDagGui()->getGui();
}

std::string
MultiInstancePanel::getName_mt_safe() const
{
    return _imp->getMainInstance()->getName_mt_safe();
}

void
MultiInstancePanel::initializeKnobs()
{
    const std::vector<boost::shared_ptr<KnobI> > & mainInstanceKnobs = _imp->getMainInstance()->getKnobs();

    for (U32 i = 0; i < mainInstanceKnobs.size(); ++i) {
        _imp->createKnob(mainInstanceKnobs[i]);
    }
    ///copy page children
    const std::vector<boost::shared_ptr<KnobI> > & knobs = getKnobs();
    for (U32 i = 0; i < knobs.size(); ++i) {
        Page_Knob* isPage = dynamic_cast<Page_Knob*>( knobs[i].get() );

        if (isPage) {
            ///find the corresponding knob in the main instance knobs
            boost::shared_ptr<KnobI> other = _imp->getMainInstance()->getKnobByName( isPage->getName() );
            assert(other);
            Page_Knob* otherPage = dynamic_cast<Page_Knob*>( other.get() );
            assert(otherPage);
            const std::vector<boost::shared_ptr<KnobI> > & otherChildren = otherPage->getChildren();
            bool isNodePage = otherPage->getName() == "Node";
            for (U32 j = 0; j < otherChildren.size(); ++j) {
                if ( !otherChildren[j]->isInstanceSpecific() ) {
                    boost::shared_ptr<KnobI> thisChild = getKnobByName( otherChildren[j]->getName() );
                    assert(thisChild);
                    isPage->addKnob(thisChild);
                    if ( isNodePage && !thisChild->isDeclaredByPlugin() ) {
                        thisChild->setAllDimensionsEnabled(false);
                    }
                }
            }
        }
    }
    initializeExtraKnobs();
}

bool
MultiInstancePanel::isGuiCreated() const
{
    return _imp->guiCreated;
}

void
MultiInstancePanel::createMultiInstanceGui(QVBoxLayout* layout)
{
    appendExtraGui(layout);
    layout->addSpacing(20);

    std::list<boost::shared_ptr<KnobI> > instanceSpecificKnobs;
    _imp->getInstanceSpecificKnobs(_imp->getMainInstance().get(), &instanceSpecificKnobs);

    _imp->view = new TableView( layout->parentWidget() );
    QObject::connect( _imp->view,SIGNAL( deleteKeyPressed() ),this,SLOT( onDeleteKeyPressed() ) );
    QObject::connect( _imp->view,SIGNAL( itemRightClicked(TableItem*) ),this,SLOT( onItemRightClicked(TableItem*) ) );
    TableItemDelegate* delegate = new TableItemDelegate(_imp->view,this);
    _imp->view->setItemDelegate(delegate);

    _imp->model = new TableModel(0,0,_imp->view);
    QObject::connect( _imp->model,SIGNAL( s_itemChanged(TableItem*) ),this,SLOT( onItemDataChanged(TableItem*) ) );
    _imp->view->setTableModel(_imp->model);

    QItemSelectionModel *selectionModel = _imp->view->selectionModel();
    QObject::connect( selectionModel, SIGNAL( selectionChanged(QItemSelection,QItemSelection) ),this,
                      SLOT( onSelectionChanged(QItemSelection,QItemSelection) ) );
    QStringList dimensionNames;
    for (std::list<boost::shared_ptr<KnobI> >::iterator it = instanceSpecificKnobs.begin(); it != instanceSpecificKnobs.end(); ++it) {
        QString knobDesc( (*it)->getDescription().c_str() );
        int dims = (*it)->getDimension();
        for (int i = 0; i < dims; ++i) {
            QString dimName(knobDesc);
            if (dims > 1) {
                dimName += ' ';
                dimName += (*it)->getDimensionName(i).c_str();
            }
            dimensionNames.push_back(dimName);
        }
    }
    dimensionNames.prepend("Enabled");

    _imp->view->setColumnCount( dimensionNames.size() );
    _imp->view->setHorizontalHeaderLabels(dimensionNames);

    _imp->view->setAttribute(Qt::WA_MacShowFocusRect,0);

#if QT_VERSION < 0x050000
    _imp->view->header()->setResizeMode(QHeaderView::ResizeToContents);
#else
    _imp->view->header()->setSectionResizeMode(QHeaderView::ResizeToContents);
#endif
    _imp->view->header()->setStretchLastSection(true);


    layout->addWidget(_imp->view);

    _imp->buttonsContainer = new QWidget( layout->parentWidget() );
    _imp->buttonsLayout = new QHBoxLayout(_imp->buttonsContainer);
    _imp->buttonsLayout->setContentsMargins(0, 0, 0, 0);
    _imp->addButton = new Button(QIcon(),"+",_imp->buttonsContainer);
    _imp->addButton->setToolTip("Add new");
    _imp->buttonsLayout->addWidget(_imp->addButton);
    QObject::connect( _imp->addButton, SIGNAL( clicked(bool) ), this, SLOT( onAddButtonClicked() ) );

    _imp->removeButton = new Button(QIcon(),"-",_imp->buttonsContainer);
    _imp->removeButton->setToolTip( tr("Remove selection") );
    _imp->buttonsLayout->addWidget(_imp->removeButton);
    QObject::connect( _imp->removeButton, SIGNAL( clicked(bool) ), this, SLOT( onRemoveButtonClicked() ) );
    QPixmap selectAll;
    appPTR->getIcon(NATRON_PIXMAP_SELECT_ALL, &selectAll);

    _imp->selectAll = new Button(QIcon(selectAll),"",_imp->buttonsContainer);
    _imp->selectAll->setFixedSize(NATRON_SMALL_BUTTON_SIZE,NATRON_SMALL_BUTTON_SIZE);
    _imp->selectAll->setToolTip( tr("Select all") );
    _imp->buttonsLayout->addWidget(_imp->selectAll);
    QObject::connect( _imp->selectAll, SIGNAL( clicked(bool) ), this, SLOT( onSelectAllButtonClicked() ) );

    _imp->resetTracksButton = new Button("Reset",_imp->buttonsContainer);
    QObject::connect( _imp->resetTracksButton, SIGNAL( clicked(bool) ), this, SLOT( resetSelectedInstances() ) );
    _imp->buttonsLayout->addWidget(_imp->resetTracksButton);
    _imp->resetTracksButton->setToolTip( tr("Reset selected items") );

    layout->addWidget(_imp->buttonsContainer);
    appendButtons(_imp->buttonsLayout);
    _imp->buttonsLayout->addStretch();

    ///Deactivate the main-instance since this is more convenient this way for the user.
    _imp->getMainInstance()->deactivate(std::list<Natron::Node* >(),false,false,false,false);
    _imp->guiCreated = true;
} // createMultiInstanceGui

class AddNodeCommand
    : public QUndoCommand
{
    bool _firstRedoCalled;
    boost::shared_ptr<Node> _node;
    MultiInstancePanel* _panel;

public:

    AddNodeCommand(MultiInstancePanel* panel,
                   const boost::shared_ptr<Node> & node,
                   QUndoCommand* parent = 0)
        : QUndoCommand(parent)
          , _firstRedoCalled(false)
          , _node(node)
          , _panel(panel)
    {
    }

    virtual ~AddNodeCommand()
    {
    }

    virtual void undo() OVERRIDE FINAL
    {
        int index = _panel->getNodeIndex(_node);

        assert(index != -1);
        _panel->removeRow(index);
        _node->deactivate();
        _panel->getMainInstance()->getApp()->redrawAllViewers();
        setText( QObject::tr("Add %1").arg( _node->getName().c_str() ) );
    }

    virtual void redo() OVERRIDE FINAL
    {
        if (_firstRedoCalled) {
            _node->activate();
            _panel->addRow(_node);
        }
        _panel->getMainInstance()->getApp()->redrawAllViewers();
        _firstRedoCalled = true;
        setText( QObject::tr("Add %1").arg( _node->getName().c_str() ) );
    }
};

boost::shared_ptr<Natron::Node> MultiInstancePanel::createNewInstance(bool useUndoRedoStack)
{
    return addInstanceInternal(useUndoRedoStack);
}

void
MultiInstancePanel::onAddButtonClicked()
{
    ignore_result(addInstanceInternal(true));
}

boost::shared_ptr<Natron::Node> MultiInstancePanel::addInstanceInternal(bool useUndoRedoStack)
{
    boost::shared_ptr<Natron::Node> mainInstance = _imp->getMainInstance();
    CreateNodeArgs args( mainInstance->getPluginID().c_str(),
                         mainInstance->getName(),
                         -1,-1,
                         (int)_imp->instances.size(),
                        true,
                        INT_MIN,INT_MIN,
                        false,  //< never use the undo-stack of the nodegraph since we use the one of the dockablepanel
                        true,
                        QString(),
                        CreateNodeArgs::DefaultValuesList());
    boost::shared_ptr<Node> newInstance = _imp->getMainInstance()->getApp()->createNode(args);

    _imp->addTableRow(newInstance);
    
    if (useUndoRedoStack) {
        _imp->pushUndoCommand( new AddNodeCommand(this,newInstance) );
    }

    return newInstance;
}

const std::list< std::pair<boost::shared_ptr<Natron::Node>,bool> > &
MultiInstancePanel::getInstances() const
{
    assert( QThread::currentThread() == qApp->thread() );

    return _imp->instances;
}

void
MultiInstancePanel::addRow(const boost::shared_ptr<Natron::Node> & node)
{
    _imp->addTableRow(node);
}

void
MultiInstancePanelPrivate::addTableRow(const boost::shared_ptr<Natron::Node> & node)
{
    instances.push_back( std::make_pair(node,false) );
    int newRowIndex = view->rowCount();
    model->insertRow(newRowIndex);

    std::list<boost::shared_ptr<KnobI> > instanceSpecificKnobs;
    {
        const std::vector<boost::shared_ptr<KnobI> > & instanceKnobs = node->getKnobs();
        for (U32 i = 0; i < instanceKnobs.size(); ++i) {
            boost::shared_ptr<KnobSignalSlotHandler> slotsHandler =
                instanceKnobs[i]->getSignalSlotHandler();
            if (slotsHandler) {
                QObject::connect( slotsHandler.get(), SIGNAL( valueChanged(int,int) ), publicInterface,SLOT( onInstanceKnobValueChanged(int,int) ) );
            }

            if ( instanceKnobs[i]->isInstanceSpecific() ) {
                Int_Knob* isInt = dynamic_cast<Int_Knob*>( instanceKnobs[i].get() );
                Bool_Knob* isBool = dynamic_cast<Bool_Knob*>( instanceKnobs[i].get() );
                Double_Knob* isDouble = dynamic_cast<Double_Knob*>( instanceKnobs[i].get() );
                Color_Knob* isColor = dynamic_cast<Color_Knob*>( instanceKnobs[i].get() );
                String_Knob* isString = dynamic_cast<String_Knob*>( instanceKnobs[i].get() );
                if (!isInt && !isBool && !isDouble && !isColor && !isString) {
                    qDebug() << "Multi-instance panel doesn't support the following type of knob: " << instanceKnobs[i]->typeName().c_str();
                    continue;
                }

                instanceSpecificKnobs.push_back(instanceKnobs[i]);
            }
        }
    }


    ///first add the enabled column
    {
        AnimatedCheckBox* checkbox = new AnimatedCheckBox();
        QObject::connect( checkbox,SIGNAL( toggled(bool) ),publicInterface,SLOT( onCheckBoxChecked(bool) ) );
        checkbox->setChecked( !node->isNodeDisabled() );
        view->setCellWidget(newRowIndex, 0, checkbox);
        TableItem* newItem = new TableItem;
        newItem->setFlags(Qt::ItemIsEnabled | Qt::ItemIsSelectable | Qt::ItemIsEditable | Qt::ItemIsUserCheckable);
        view->setItem(newRowIndex, 0, newItem);
        view->resizeColumnToContents(0);
    }
    int columnIndex = 1;
    for (std::list<boost::shared_ptr<KnobI> >::iterator it = instanceSpecificKnobs.begin(); it != instanceSpecificKnobs.end(); ++it) {
        Int_Knob* isInt = dynamic_cast<Int_Knob*>( it->get() );
        Bool_Knob* isBool = dynamic_cast<Bool_Knob*>( it->get() );
        Double_Knob* isDouble = dynamic_cast<Double_Knob*>( it->get() );
        Color_Knob* isColor = dynamic_cast<Color_Knob*>( it->get() );
        String_Knob* isString = dynamic_cast<String_Knob*>( it->get() );


        ///Only these types are supported
        if (!isInt && !isBool && !isDouble && !isColor && !isString) {
            continue;
        }

        for (int i = 0; i < (*it)->getDimension(); ++i) {
            TableItem* newItem = new TableItem;
            Qt::ItemFlags flags = Qt::ItemIsEnabled | Qt::ItemIsSelectable | Qt::ItemIsEditable;

            if (isBool) {
                bool checked = isBool->getValue(i);
                AnimatedCheckBox* checkbox = new AnimatedCheckBox();
                checkbox->setChecked(checked);
                view->setCellWidget(newRowIndex, columnIndex, checkbox);
                flags |= Qt::ItemIsUserCheckable;
            } else if (isInt) {
                newItem->setData( Qt::DisplayRole, isInt->getValue(i) );
            } else if (isDouble) {
                newItem->setData( Qt::DisplayRole, isDouble->getValue(i) );
            } else if (isString) {
                newItem->setData( Qt::DisplayRole, isString->getValue(i).c_str() );
            }
            newItem->setFlags(flags);

            view->setItem(newRowIndex, columnIndex, newItem);
            view->resizeColumnToContents(columnIndex);
            ++columnIndex;
        }
    }

    ///clear current selection
    view->selectionModel()->clear();

    ///select the new item
    QModelIndex newIndex = model->index(newRowIndex, 0);
    assert( newIndex.isValid() );
    view->selectionModel()->select(newIndex, QItemSelectionModel::ClearAndSelect | QItemSelectionModel::Rows);
} // addTableRow

void
MultiInstancePanel::selectNode(const boost::shared_ptr<Natron::Node> & node,
                               bool addToSelection)
{
    if (!addToSelection) {
        _imp->view->selectionModel()->clear();
    }

    int index = -1;
    int i = 0;
    for (std::list< std::pair<boost::shared_ptr<Node>,bool > >::iterator it = _imp->instances.begin(); it != _imp->instances.end(); ++it,++i) {
        if (it->first == node) {
            index = i;
            break;
        }
    }
    assert(index != -1);

    QItemSelection newSelection( _imp->model->index(index, 0),_imp->model->index(index,_imp->view->columnCount() - 1) );
    _imp->view->selectionModel()->select(newSelection, QItemSelectionModel::Select);
}

void
MultiInstancePanel::removeNodeFromSelection(const boost::shared_ptr<Natron::Node> & node)
{
    int index = -1;
    int i = 0;

    for (std::list< std::pair<boost::shared_ptr<Node>,bool > >::iterator it = _imp->instances.begin(); it != _imp->instances.end(); ++it,++i) {
        if (it->first == node) {
            index = i;
            break;
        }
    }
    assert(index != -1);
    QItemSelection newSelection( _imp->model->index(index, 0),_imp->model->index(index,_imp->view->columnCount() - 1) );
    _imp->view->selectionModel()->select(newSelection, QItemSelectionModel::Deselect);
}

void
MultiInstancePanel::clearSelection()
{
    _imp->view->selectionModel()->clear();
}

void
MultiInstancePanel::selectNodes(const std::list<Natron::Node*> & nodes,
                                bool addToSelection)
{
    //_imp->view->selectionModel()->blockSignals(true);
    if (!addToSelection) {
        _imp->view->clearSelection();
    }
//    for (std::list< std::pair<boost::shared_ptr<Node>,bool > >::iterator it2 = _imp->instances.begin();
//         it2!=_imp->instances.end(); ++it2) {
//        it2->second = false;
//    }
//    _imp->view->selectionModel()->blockSignals(false);
    if ( nodes.empty() ) {
        return;
    }


    QItemSelection newSelection;
    for (std::list<Natron::Node*>::const_iterator it = nodes.begin(); it != nodes.end(); ++it) {
        int i = 0;
        for (std::list< std::pair<boost::shared_ptr<Node>,bool > >::iterator it2 = _imp->instances.begin();
             it2 != _imp->instances.end(); ++it2,++i) {
            if (it2->first.get() == *it) {
                QItemSelection sel( _imp->model->index(i, 0),_imp->model->index(i,_imp->view->columnCount() - 1) );
                newSelection.merge(sel, QItemSelectionModel::Select);
                break;
            }
        }
    }
    _imp->view->selectionModel()->select(newSelection, QItemSelectionModel::Select);
}

class RemoveNodeCommand
    : public QUndoCommand
{
    MultiInstancePanel* _panel;
    std::list<boost::shared_ptr<Natron::Node> > _nodes;

public:

    RemoveNodeCommand(MultiInstancePanel* panel,
                      const std::list<boost::shared_ptr<Natron::Node> > & nodes,
                      QUndoCommand* parent = 0)
        : QUndoCommand(parent)
          , _panel(panel)
          , _nodes(nodes)
    {
    }

    virtual ~RemoveNodeCommand()
    {
    }

    virtual void undo() OVERRIDE FINAL
    {
        
        _panel->addInstances(_nodes);
        _panel->getMainInstance()->getApp()->triggerAutoSave();
        _panel->getMainInstance()->getApp()->redrawAllViewers();
        setText( QObject::tr("Remove instance(s)") );
    }

    virtual void redo() OVERRIDE FINAL
    {
        _panel->removeInstances(_nodes);
        _panel->getMainInstance()->getApp()->triggerAutoSave();
        _panel->getMainInstance()->getApp()->redrawAllViewers();
        setText( QObject::tr("Remove instance(s)") );
    }
};

void
MultiInstancePanel::removeInstances(const std::list<boost::shared_ptr<Natron::Node> >& instances)
{
    boost::shared_ptr<Node> mainInstance = getMainInstance();
    std::list<boost::shared_ptr<Natron::Node> >::const_iterator next = instances.begin();
    if (!instances.empty()) {
	   ++next;
	}
   
    for (std::list<boost::shared_ptr<Natron::Node> >::const_iterator it = instances.begin(); it != instances.end(); ++it,++next) {
        int index = getNodeIndex(*it);
        assert(index != -1);
        removeRow(index);
        bool isMainInstance = (*it) == mainInstance;
        (*it)->deactivate( std::list<Natron::Node* >(),false,false,!isMainInstance,next == instances.end() );
		if (next == instances.end()) {
			--next;
		}
    }
    

}

void
MultiInstancePanel::addInstances(const std::list<boost::shared_ptr<Natron::Node> >& instances)
{
    std::list<boost::shared_ptr<Natron::Node> >::const_iterator next = instances.begin();
    if (!instances.empty()) {
		++next;
	}
    for (std::list<boost::shared_ptr<Natron::Node> >::const_iterator it = instances.begin(); it != instances.end(); ++it,++next) {
        addRow(*it);
        (*it)->activate( std::list<Natron::Node* >(),false,next == instances.end() );
		if (next == instances.end()) {
			--next;
		}
    }
}

void
MultiInstancePanel::removeRow(int index)
{
    _imp->removeRow(index);
}

void
MultiInstancePanelPrivate::removeRow(int index)
{
    if ( (index < 0) || ( index >= (int)instances.size() ) ) {
        throw std::invalid_argument("Index out of range");
    }
    model->removeRows(index);
    Nodes::iterator it = instances.begin();
    std::advance(it, index);
    instances.erase(it);
}

int
MultiInstancePanel::getNodeIndex(const boost::shared_ptr<Natron::Node> & node) const
{
    int i = 0;
    Nodes::iterator it = _imp->instances.begin();

    for (; it != _imp->instances.end(); ++it,++i) {
        if (it->first == node) {
            return i;
        }
    }

    return -1;
}

void
MultiInstancePanel::onDeleteKeyPressed()
{
    removeInstancesInternal();
}

void
MultiInstancePanel::onRemoveButtonClicked()
{
    removeInstancesInternal();
}

void
MultiInstancePanel::removeInstancesInternal()
{
    const QItemSelection selection = _imp->view->selectionModel()->selection();
    std::list<boost::shared_ptr<Node> > instances;
    QModelIndexList indexes = selection.indexes();
    std::set<int> rows;

    for (int i = 0; i < indexes.size(); ++i) {
        rows.insert( indexes[i].row() );
    }

    for (std::set<int>::iterator it = rows.begin(); it != rows.end(); ++it) {
        assert( *it >= 0 && *it < (int)_imp->instances.size() );
        std::list< std::pair<boost::shared_ptr<Node>,bool > >::iterator it2 = _imp->instances.begin();
        std::advance(it2, *it);
        instances.push_back(it2->first);
    }
    _imp->pushUndoCommand( new RemoveNodeCommand(this,instances) );
}

void
MultiInstancePanel::onSelectAllButtonClicked()
{
    _imp->view->selectAll();
}

bool
MultiInstancePanel::isSettingsPanelVisible() const
{
    NodeSettingsPanel* panel = _imp->mainInstance->getSettingPanel();

    assert(panel);

    return !panel->isClosed();
}


void
MultiInstancePanel::onSettingsPanelClosed(bool closed)
{
    std::list<Node*> selection;

    getSelectedInstances(&selection);

    std::list<Node*>::iterator next = selection.begin();
	if (!selection.empty()) {
		++next;
	}
    for (std::list<Node*>::iterator it = selection.begin(); it != selection.end(); ++it,++next) {
        if (closed) {
            (*it)->hideKeyframesFromTimeline( next == selection.end() );
        } else {
            (*it)->showKeyframesOnTimeline( next == selection.end() );
        }
		if (next == selection.end()) {
			--next;
		}
    }
}

void
MultiInstancePanel::onSelectionChanged(const QItemSelection & newSelection,
                                       const QItemSelection & oldSelection)
{
    std::list<std::pair<Node*,bool> > previouslySelectedInstances;
    QModelIndexList oldIndexes = oldSelection.indexes();

    _imp->getNodesFromSelection(oldIndexes, &previouslySelectedInstances);

    bool copyOnUnSlave = previouslySelectedInstances.size()  <= 1;

    /// new selection
    std::list<std::pair<Node*,bool> > newlySelectedInstances;
    QModelIndexList newIndexes = newSelection.indexes();
    for (int i = 0; i < newIndexes.size(); ++i) {
        TableItem* item = _imp->model->item(newIndexes[i]);
        if (item) {
            item->setFlags(item->flags() | Qt::ItemIsEditable);
        }
    }
    _imp->getNodesFromSelection(newIndexes, &newlySelectedInstances);

    ///Don't consider items that are in both previouslySelectedInstances && newlySelectedInstances
    
    QModelIndexList rows = _imp->view->selectionModel()->selectedRows();
    bool setDirty = rows.count() > 1;
    std::list<std::pair<Node*,bool> >::iterator nextPreviouslySelected = previouslySelectedInstances.begin();
	if (!previouslySelectedInstances.empty()) {
		++nextPreviouslySelected;
	}
    
    
    for (std::list<std::pair<Node*,bool> >::iterator it = previouslySelectedInstances.begin();
         it != previouslySelectedInstances.end(); ++it,++nextPreviouslySelected) {
        ///if the item is in the new selection, don't consider it
        bool skip = false;
        for (std::list<std::pair<Node*,bool> >::iterator it2 = newlySelectedInstances.begin();
             it2 != newlySelectedInstances.end(); ++it2) {
            if (it2->first == it->first) {
                skip = true;
                break;
            }
        }
        ///disconnect all the knobs
        if (!it->second || skip) {
            continue;
        }

        it->first->hideKeyframesFromTimeline( nextPreviouslySelected == previouslySelectedInstances.end() );
        
        it->first->getLiveInstance()->beginChanges();
        const std::vector<boost::shared_ptr<KnobI> > & knobs = it->first->getKnobs();
        for (U32 i = 0; i < knobs.size(); ++i) {
            if ( knobs[i]->isDeclaredByPlugin() && !knobs[i]->isInstanceSpecific() && !knobs[i]->getIsSecret() ) {
                for (int j = 0; j < knobs[i]->getDimension(); ++j) {
                    if ( knobs[i]->isSlave(j) ) {
                        knobs[i]->unSlave(j, copyOnUnSlave);
                    }
                }
            }
        }
        it->first->getLiveInstance()->endChanges();

        for (Nodes::iterator it2 = _imp->instances.begin(); it2 != _imp->instances.end(); ++it2) {
            if (it2->first.get() == it->first) {
                it2->second = false;
                break;
            }
        }
		if (nextPreviouslySelected == previouslySelectedInstances.end()) {
			--nextPreviouslySelected;
		}
    }
    
    std::list<SequenceTime> allKeysToAdd;
    std::list<std::pair<Node*,bool> >::iterator nextNewlySelected = newlySelectedInstances.begin();
	if (!newlySelectedInstances.empty()) {
		++nextNewlySelected;
	}
    for (std::list<std::pair<Node*,bool> >::iterator it = newlySelectedInstances.begin();
         it != newlySelectedInstances.end(); ++it,++nextNewlySelected) {
        ///if the item is in the old selection, don't consider it
        bool skip = false;
        for (std::list<std::pair<Node*,bool> >::iterator it2 = previouslySelectedInstances.begin();
             it2 != previouslySelectedInstances.end(); ++it2) {
            if (it2->first == it->first) {
                skip = true;
                break;
            }
        }

        if (it->second || skip) {
            continue;
        }

        if ( isSettingsPanelVisible() ) {
            it->first->showKeyframesOnTimeline( nextNewlySelected == newlySelectedInstances.end() );
        }

        ///slave all the knobs that are declared by the plug-in (i.e: not the ones from the "Node" page)
        //and which are not instance specific (not the knob displayed in the table)
        const std::vector<boost::shared_ptr<KnobI> > & knobs = it->first->getKnobs();
        for (U32 i = 0; i < knobs.size(); ++i) {
            if ( knobs[i]->isDeclaredByPlugin() && !knobs[i]->isInstanceSpecific() && !knobs[i]->getIsSecret() ) {
                boost::shared_ptr<KnobI> otherKnob = getKnobByName( knobs[i]->getName() );
                assert(otherKnob);

                ///Don't slave knobs when several are selected otherwise all the instances would then share the same values
                ///while being selected
                if (!setDirty) {
                    ///do not slave buttons, handle them separatly in onButtonTriggered()
                    Button_Knob* isButton = dynamic_cast<Button_Knob*>( knobs[i].get() );
                    if (!isButton) {
                        otherKnob->clone(knobs[i]);
                        knobs[i]->beginChanges();
                        for (int j = 0; j < knobs[i]->getDimension(); ++j) {
                            knobs[i]->slaveTo(j, otherKnob, j,true);
                        }
                        knobs[i]->endChanges();
                    }
                }

                otherKnob->setAllDimensionsEnabled(true);
                otherKnob->setDirty(setDirty);
            }
        }
        for (Nodes::iterator it2 = _imp->instances.begin(); it2 != _imp->instances.end(); ++it2) {
            if (it2->first.get() == it->first) {
                it2->second = true;
                break;
            }
        }

		if (nextNewlySelected == newlySelectedInstances.end()) {
			--nextNewlySelected;
		}
    }


    if ( newlySelectedInstances.empty() ) {
        ///disable knobs
        const std::vector<boost::shared_ptr<KnobI> > & knobs = getKnobs();
        for (U32 i = 0; i < knobs.size(); ++i) {
            if ( knobs[i]->isDeclaredByPlugin() && !knobs[i]->isInstanceSpecific() ) {
                knobs[i]->setAllDimensionsEnabled(false);
                knobs[i]->setDirty(false);
            }
        }
    }
    
    getGui()->redrawAllViewers();
} // onSelectionChanged

void
MultiInstancePanelPrivate::getNodesFromSelection(const QModelIndexList & indexes,
                                                 std::list<std::pair<Node*,bool> >* nodes)
{
    std::set<int> rows;

    for (int i = 0; i < indexes.size(); ++i) {
        rows.insert( indexes[i].row() );
    }

    for (std::set<int>::iterator it = rows.begin(); it != rows.end(); ++it) {
        assert( *it >= 0 && *it < (int)instances.size() );
        std::list< std::pair<boost::shared_ptr<Node>,bool > >::iterator it2 = instances.begin();
        std::advance(it2, *it);
        if ( !it2->first->isNodeDisabled() ) {
            nodes->push_back( std::make_pair(it2->first.get(), it2->second) );
        }
    }
}

boost::shared_ptr<Natron::Node>
MultiInstancePanelPrivate::getInstanceFromItem(TableItem* item) const
{
    assert( item->row() >= 0 && item->row() < (int)instances.size() );
    int i = 0;
    for (std::list< std::pair<boost::shared_ptr<Node>,bool > >::const_iterator it = instances.begin(); it != instances.end(); ++it,++i) {
        if ( i == item->row() ) {
            return it->first;
        }
    }

    return boost::shared_ptr<Natron::Node>();
}

boost::shared_ptr<KnobI> MultiInstancePanel::getKnobForItem(TableItem* item,
                                                            int* dimension) const
{
    QModelIndex modelIndex = _imp->model->index(item);

    assert( modelIndex.row() < (int)_imp->instances.size() );
    Nodes::iterator nIt = _imp->instances.begin();
    std::advance( nIt, modelIndex.row() );
    const std::vector<boost::shared_ptr<KnobI> > & knobs = nIt->first->getKnobs();
    int instanceSpecificIndex = 1; //< 1 because we skip the enable cell
    for (U32 i = 0; i < knobs.size(); ++i) {
        if ( knobs[i]->isInstanceSpecific() ) {
            for (int j = 0; j < knobs[i]->getDimension(); ++j) {
                if ( instanceSpecificIndex == modelIndex.column() ) {
                    *dimension = j;

                    return knobs[i];
                }
                ++instanceSpecificIndex;
            }
        }
    }
    *dimension = -1;

    return boost::shared_ptr<KnobI>();
}

void
MultiInstancePanel::onItemDataChanged(TableItem* item)
{
    if (_imp->executingKnobValueChanged) {
        return;
    }
    QVariant data = item->data(Qt::DisplayRole);
    QModelIndex modelIndex = _imp->model->index(item);

    ///The enabled cell is handled in onCheckBoxChecked
    if (modelIndex.column() == 0) {
        return;
    }
    
    int time = getApp()->getTimeLine()->currentFrame();
    
    assert( modelIndex.row() < (int)_imp->instances.size() );
    Nodes::iterator nIt = _imp->instances.begin();
    std::advance( nIt, modelIndex.row() );
    
    if (modelIndex.column() == 1) {
        nIt->first->setName(data.toString());
    }
    
    const std::vector<boost::shared_ptr<KnobI> > & knobs = nIt->first->getKnobs();
    int instanceSpecificIndex = 1; //< 1 because we skip the enable cell
    for (U32 i = 0; i < knobs.size(); ++i) {
        if ( knobs[i]->isInstanceSpecific() ) {
            for (int j = 0; j < knobs[i]->getDimension(); ++j) {
                if ( instanceSpecificIndex == modelIndex.column() ) {

                    Int_Knob* isInt = dynamic_cast<Int_Knob*>( knobs[i].get() );
                    Bool_Knob* isBool = dynamic_cast<Bool_Knob*>( knobs[i].get() );
                    Double_Knob* isDouble = dynamic_cast<Double_Knob*>( knobs[i].get() );
                    Color_Knob* isColor = dynamic_cast<Color_Knob*>( knobs[i].get() );
                    String_Knob* isString = dynamic_cast<String_Knob*>( knobs[i].get() );
                    
                    if (knobs[i]->isAnimationEnabled()) {
                        if (isInt) {
                            isInt->setValueAtTime(time, data.toInt(), j);
                        } else if (isBool) {
                            isBool->setValueAtTime(time, data.toBool(), j);
                        } else if (isDouble) {
                            isDouble->setValueAtTime(time, data.toDouble(), j);
                        } else if (isColor) {
                            isColor->setValueAtTime(time, data.toDouble(), j);
                        } else if (isString) {
                            isString->setValueAtTime(time, data.toString().toStdString(), j);
                        }
                    } else {
                        if (isInt) {
                            isInt->setValue(data.toInt(), j, true);
                        } else if (isBool) {
                            isBool->setValue(data.toBool(), j, true);
                        } else if (isDouble) {
                            isDouble->setValue(data.toDouble(), j, true);
                        } else if (isColor) {
                            isColor->setValue(data.toDouble(), j, true);
                        } else if (isString) {
                            isString->setValue(data.toString().toStdString(), j, true);
                        }
                    }
                    return;
                }
                ++instanceSpecificIndex;
            }
        }
    }
}

void
MultiInstancePanel::onItemRightClicked(TableItem* item)
{
    boost::shared_ptr<Natron::Node> instance = _imp->getInstanceFromItem(item);

    if (instance) {
        showMenuForInstance( instance.get() );
    }
}

///The checkbox interacts directly with the kDisableNodeKnobName knob of the node
///It doesn't call deactivate() on the node so calling isActivated() on a node
///will still return true even if you set the value of kDisableNodeKnobName to false.
void
MultiInstancePanel::onCheckBoxChecked(bool checked)
{
    AnimatedCheckBox* checkbox = qobject_cast<AnimatedCheckBox*>( sender() );

    if (!checkbox) {
        return;
    }

    ///find the row which owns this checkbox
    for (int i = 0; i < _imp->model->rowCount(); ++i) {
        QWidget* w = _imp->view->cellWidget(i, 0);
        if (w == checkbox) {
            assert( i < (int)_imp->instances.size() );
            Nodes::iterator it = _imp->instances.begin();
            std::advance(it, i);
            boost::shared_ptr<KnobI> enabledKnob = it->first->getKnobByName(kDisableNodeKnobName);
            assert(enabledKnob);
            Bool_Knob* bKnob = dynamic_cast<Bool_Knob*>( enabledKnob.get() );
            assert(bKnob);
            bKnob->setValue(!checked, 0);
            break;
        }
    }
    getApp()->redrawAllViewers();
}

void
MultiInstancePanel::onInstanceKnobValueChanged(int dim,
                                               int reason)
{
    if ( (Natron::ValueChangedReasonEnum)reason == Natron::eValueChangedReasonSlaveRefresh ) {
        return;
    }

    KnobSignalSlotHandler* signalEmitter = qobject_cast<KnobSignalSlotHandler*>( sender() );
    if (!signalEmitter) {
        return;
    }
    boost::shared_ptr<KnobI> knob = signalEmitter->getKnob();
    if ( !knob->isDeclaredByPlugin() ) {
        return;
    }
    KnobHolder* holder = knob->getHolder();
    assert(holder);
    int rowIndex = 0;
    int colIndex = 1;
    for (Nodes::iterator it = _imp->instances.begin(); it != _imp->instances.end(); ++it,++rowIndex) {
        if ( holder == it->first->getLiveInstance() ) {
            const std::vector<boost::shared_ptr<KnobI> > & knobs = it->first->getKnobs();
            for (U32 i = 0; i < knobs.size(); ++i) {
                if ( knobs[i]->isInstanceSpecific() ) {
                    if (knobs[i] == knob) {
                        colIndex += dim;
                        TableItem* item = _imp->model->item(rowIndex, colIndex);
                        if (!item) {
                            continue;
                        }
                        QVariant data;
                        Int_Knob* isInt = dynamic_cast<Int_Knob*>( knobs[i].get() );
                        Bool_Knob* isBool = dynamic_cast<Bool_Knob*>( knobs[i].get() );
                        Double_Knob* isDouble = dynamic_cast<Double_Knob*>( knobs[i].get() );
                        Color_Knob* isColor = dynamic_cast<Color_Knob*>( knobs[i].get() );
                        String_Knob* isString = dynamic_cast<String_Knob*>( knobs[i].get() );
                        if (isInt) {
                            data.setValue<int>( isInt->getValue(dim) );
                        } else if (isBool) {
                            data.setValue<bool>( isBool->getValue(dim) );
                        } else if (isDouble) {
                            data.setValue<double>( isDouble->getValue(dim) );
                        } else if (isColor) {
                            data.setValue<double>( isColor->getValue(dim) );
                        } else if (isString) {
                            data.setValue<QString>( isString->getValue(dim).c_str() );
                        }
                        _imp->executingKnobValueChanged = true;
                        item->setData(Qt::DisplayRole,data);
                        _imp->executingKnobValueChanged = false;

                        return;
                    }
                    colIndex += knobs[i]->getDimension();
                } else if ( (knobs[i] == knob) && !_imp->knobValueRecursion ) {
                    ///If the knob is slaved to a knob used only for GUI, unslave it before updating value and reslave back
                    std::pair<int,boost::shared_ptr<KnobI> > master = knob->getMaster(dim);
                    if (master.second) {
                        ++_imp->knobValueRecursion;
                        knob->unSlave(dim, false);
                        Knob<int>* isInt = dynamic_cast<Knob<int>*>( knob.get() );
                        Knob<bool>* isBool = dynamic_cast<Knob<bool>*>( knob.get() );
                        Knob<double>* isDouble = dynamic_cast<Knob<double>*>( knob.get() );
                        Knob<std::string>* isString = dynamic_cast<Knob<std::string>*>( knob.get() );
                        if (isInt) {
                            Knob<int>* masterKnob = dynamic_cast<Knob<int>*>( master.second.get() );
                            assert(masterKnob);
                            if (masterKnob) {
                                masterKnob->clone( knob.get() );
                            }
                        } else if (isBool) {
                            Knob<bool>* masterKnob = dynamic_cast<Knob<bool>*>( master.second.get() );
                            assert(masterKnob);
                            if (masterKnob) {
                                masterKnob->clone( knob.get() );
                            }
                        } else if (isDouble) {
                            Knob<double>* masterKnob = dynamic_cast<Knob<double>*>( master.second.get() );
                            assert(masterKnob);
                            if (masterKnob) {
                                masterKnob->clone( knob.get() );
                            }
                        } else if (isString) {
                            Knob<std::string>* masterKnob = dynamic_cast<Knob<std::string>*>( master.second.get() );
                            assert(masterKnob);
                            if (masterKnob) {
                                masterKnob->clone( knob.get() );
                            }
                        }
                        knob->slaveTo(dim, master.second, master.first,true);
                        --_imp->knobValueRecursion;
                    }
                }
            }

            return;
        }
    }
} // onInstanceKnobValueChanged

void
MultiInstancePanel::getSelectedInstances(std::list<Natron::Node*>* instances) const
{
    const QItemSelection selection = _imp->view->selectionModel()->selection();
    QModelIndexList indexes = selection.indexes();
    std::set<int> rows;

    for (int i = 0; i < indexes.size(); ++i) {
        rows.insert( indexes[i].row() );
    }

    for (std::set<int>::iterator it = rows.begin(); it != rows.end(); ++it) {
        assert( *it >= 0 && *it < (int)_imp->instances.size() );
        std::list< std::pair<boost::shared_ptr<Node>,bool > >::iterator it2 = _imp->instances.begin();
        std::advance(it2, *it);
        instances->push_back( it2->first.get() );
    }
}

void
MultiInstancePanel::resetSelectedInstances()
{
    std::list<Natron::Node*> selectedInstances;

    getSelectedInstances(&selectedInstances);
    _imp->view->selectionModel()->clear();
    resetInstances(selectedInstances);
}

void
MultiInstancePanel::resetAllInstances()
{
    _imp->view->selectionModel()->clear();
    std::list<Natron::Node*> all;
    for (Nodes::iterator it = _imp->instances.begin(); it != _imp->instances.end(); ++it) {
        all.push_back( it->first.get() );
    }
    resetInstances(all);
}

void
MultiInstancePanel::resetInstances(const std::list<Natron::Node*> & instances)
{
    if ( instances.empty() ) {
        return;
    }

    std::list<Natron::Node*>::const_iterator next = instances.begin();
    ++next;
    for (std::list<Natron::Node*>::const_iterator it = instances.begin(); it != instances.end(); ++it,++next) {
        //invalidate the cache by incrementing the age
        (*it)->incrementKnobsAge();
        if ( (*it)->areKeyframesVisibleOnTimeline() ) {
            (*it)->hideKeyframesFromTimeline( next == instances.end() );
        }
        const std::vector<boost::shared_ptr<KnobI> > & knobs = (*it)->getKnobs();
        for (U32 i = 0; i < knobs.size(); ++i) {
            Button_Knob* isBtn = dynamic_cast<Button_Knob*>( knobs[i].get() );

            if ( !isBtn && (knobs[i]->getName() != kUserLabelKnobName) && (knobs[i]->getName() != kNatronOfxParamStringSublabelName) ) {
                knobs[i]->beginChanges();
                int dims = knobs[i]->getDimension();
                for (int j = 0; j < dims; ++j) {
                    knobs[i]->resetToDefaultValue(j);
                }
                knobs[i]->endChanges();
            }
        }
    }
    instances.front()->getLiveInstance()->evaluate_public(NULL, true, Natron::eValueChangedReasonUserEdited);

    ///To update interacts, kinda hack but can't figure out where else put this
    getMainInstance()->getApp()->redrawAllViewers();
}

void
MultiInstancePanel::evaluate(KnobI* knob,
                             bool /*isSignificant*/,
                             Natron::ValueChangedReasonEnum reason)
{
    Button_Knob* isButton = dynamic_cast<Button_Knob*>(knob);

    if ( isButton && (reason == eValueChangedReasonUserEdited) ) {
        onButtonTriggered(isButton);
    }
}

void
MultiInstancePanel::onButtonTriggered(Button_Knob* button)
{
    std::list<Node*> selectedInstances;

    getSelectedInstances(&selectedInstances);

    ///Forward the button click event to all the selected instances
    int time = getApp()->getTimeLine()->currentFrame();
    for (std::list<Node*>::iterator it = selectedInstances.begin(); it != selectedInstances.end(); ++it) {
        boost::shared_ptr<KnobI> k = (*it)->getKnobByName( button->getName() );
        assert( k && dynamic_cast<Button_Knob*>( k.get() ) );
        (*it)->getLiveInstance()->onKnobValueChanged_public(k.get(),eValueChangedReasonUserEdited,time, true);
    }
}

void
MultiInstancePanel::onKnobValueChanged(KnobI* k,
                                       Natron::ValueChangedReasonEnum reason,
                                       SequenceTime time,
                                       bool /*originatedFromMainThread*/)
{
    if ( !k->isDeclaredByPlugin() ) {
        if (k->getName() == kDisableNodeKnobName) {
            Bool_Knob* boolKnob = dynamic_cast<Bool_Knob*>(k);
            assert(boolKnob);
            if (boolKnob) {
                _imp->mainInstance->onDisabledKnobToggled( boolKnob->getValue() );
            }
        }
    } else {
        if (reason == Natron::eValueChangedReasonUserEdited) {
            ///Buttons are already handled in evaluate()
            Button_Knob* isButton = dynamic_cast<Button_Knob*>(k);
            if (isButton) {
                return;
            }
            ///for all selected instances update the same knob because it might not be slaved (see
            ///onSelectionChanged for an explanation why)
            for (Nodes::iterator it = _imp->instances.begin(); it != _imp->instances.end(); ++it) {
                if (it->second) {
                    boost::shared_ptr<KnobI> sameKnob = it->first->getKnobByName( k->getName() );
                    assert(sameKnob);
                    Knob<int>* isInt = dynamic_cast<Knob<int>*>( sameKnob.get() );
                    Knob<bool>* isBool = dynamic_cast<Knob<bool>*>( sameKnob.get() );
                    Knob<double>* isDouble = dynamic_cast<Knob<double>*>( sameKnob.get() );
                    Knob<std::string>* isString = dynamic_cast<Knob<std::string>*>( sameKnob.get() );
                    if (isInt) {
                        isInt->clone(k);
                    } else if (isBool) {
                        isBool->clone(k);
                    } else if (isDouble) {
                        isDouble->clone(k);
                    } else if (isString) {
                        isString->clone(k);
                    }

                    sameKnob->getHolder()->onKnobValueChanged_public(sameKnob.get(), eValueChangedReasonPluginEdited,time, true);
                }
            }
        }
    }
}

namespace  {
enum ExportTransformTypeEnum
{
    eExportTransformTypeStabilize,
    eExportTransformTypeMatchMove
};
}

/////////////// Tracker panel
struct TrackerPanelPrivate
{
    TrackerPanel* publicInterface;
    Button* averageTracksButton;
    
    mutable QMutex updateViewerMutex;
    bool updateViewerOnTrackingEnabled;
    
    QLabel* exportLabel;
    QWidget* exportContainer;
    QHBoxLayout* exportLayout;
    ComboBox* exportChoice;
    Button* exportButton;
    boost::shared_ptr<Page_Knob> transformPage;
    boost::shared_ptr<Int_Knob> referenceFrame;

    
    TrackScheduler scheduler;

    

    TrackerPanelPrivate(TrackerPanel* publicInterface)
        : publicInterface(publicInterface)
          , averageTracksButton(0)
          , updateViewerMutex()
          , updateViewerOnTrackingEnabled(true)
          , exportLabel(0)
          , exportContainer(0)
          , exportLayout(0)
          , exportChoice(0)
          , exportButton(0)
          , transformPage()
          , referenceFrame()
          , scheduler(publicInterface)
    {
    }

    void createTransformFromSelection(const std::list<Node*> & selection,bool linked,ExportTransformTypeEnum type);

    void createCornerPinFromSelection(const std::list<Node*> & selection,bool linked,bool useTransformRefFrame,bool invert);
    
    bool getTrackInstancesForButton(std::list<Button_Knob*>* trackButtons,const std::string& buttonName);
};

TrackerPanel::TrackerPanel(const boost::shared_ptr<NodeGui> & node)
    : MultiInstancePanel(node)
      , _imp( new TrackerPanelPrivate(this) )
{
    QObject::connect(&_imp->scheduler, SIGNAL(trackingStarted()), this, SLOT(onTrackingStarted()));
    QObject::connect(&_imp->scheduler, SIGNAL(trackingFinished()), this, SLOT(onTrackingFinished()));
    QObject::connect(&_imp->scheduler, SIGNAL(progressUpdate(double)), this, SLOT(onTrackingProgressUpdate(double)));
}

TrackerPanel::~TrackerPanel()
{
    _imp->scheduler.quitThread();
}

void
TrackerPanel::appendExtraGui(QVBoxLayout* layout)
{
    _imp->exportLabel = new QLabel( tr("Export data"),layout->parentWidget() );
    layout->addWidget(_imp->exportLabel);
    layout->addSpacing(10);
    _imp->exportContainer = new QWidget( layout->parentWidget() );
    _imp->exportLayout = new QHBoxLayout(_imp->exportContainer);
    _imp->exportLayout->setContentsMargins(0, 0, 0, 0);

    _imp->exportChoice = new ComboBox(_imp->exportContainer);
    _imp->exportChoice->setToolTip( tr("<p><b>CornerPinOFX (Use current frame):</p></b>"
                                       "<p>Warp the image according to the relative transform using the current frame as reference.</p>"
                                       "<p><b>CornerPinOFX (Use transform ref frame):</p></b>"
                                       "<p>Warp the image according to the relative transform using the "
                                       "reference frame specified in the transform tab.</p>"
                                       "<p><b>CornerPinOFX (Stabilize):</p></b>"
                                       "<p>Transform the image so that the tracked points do not move.</p>"
//                                      "<p><b>Transform (Stabilize):</p></b>"
//                                      "<p>Transform the image so that the tracked points do not move.</p>"
//                                      "<p><b>Transform (Match-move):</p></b>"
//                                      "<p>Transform another image so that it moves to match the tracked points.</p>"
//                                      "<p>The linked versions keep a link between the new node and the track, the others just copy"
//                                      " the values.</p>"
                                       ) );
    std::vector<std::string> choices;
    std::vector<std::string> helps;

    choices.push_back(tr("CornerPinOFX (Use current frame. Linked)").toStdString());
    helps.push_back(tr("Warp the image according to the relative transform using the current frame as reference.").toStdString());
//
//    choices.push_back(tr("CornerPinOFX (Use transform ref frame. Linked)").toStdString());
//    helps.push_back(tr("Warp the image according to the relative transform using the "
//                       "reference frame specified in the transform tab.").toStdString());
    
    choices.push_back(tr("CornerPinOFX (Stabilize. Linked)").toStdString());
    helps.push_back(tr("Transform the image so that the tracked points do not move.").toStdString());

    choices.push_back( tr("CornerPinOFX (Use current frame. Copy)").toStdString() );
    helps.push_back( tr("Same as the linked version except that it copies values instead of "
                        "referencing them via a link to the track").toStdString() );
    
    choices.push_back(tr("CornerPinOFX (Stabilize. Copy)").toStdString());
    helps.push_back(tr("Same as the linked version except that it copies values instead of "
                       "referencing them via a link to the track").toStdString());

    choices.push_back( tr("CornerPinOFX (Use transform ref frame. Copy)").toStdString() );
    helps.push_back( tr("Same as the linked version except that it copies values instead of "
                        "referencing them via a link to the track").toStdString() );
    

//    choices.push_back(tr("Transform (Stabilize. Linked)").toStdString());
//    helps.push_back(tr("Transform the image so that the tracked points do not move.").toStdString());
//
//    choices.push_back(tr("Transform (Match-move. Linked)").toStdString());
//    helps.push_back(tr("Transform another image so that it moves to match the tracked points.").toStdString());
//
//    choices.push_back(tr("Transform (Stabilize. Copy)").toStdString());
//    helps.push_back(tr("Same as the linked version except that it copies values instead of "
//                       "referencing them via a link to the track").toStdString());
//
//    choices.push_back(tr("Transform (Match-move. Copy)").toStdString());
//    helps.push_back(tr("Same as the linked version except that it copies values instead of "
//                       "referencing them via a link to the track").toStdString());
    for (U32 i = 0; i < choices.size(); ++i) {
        _imp->exportChoice->addItem( choices[i].c_str(),QIcon(),QKeySequence(),helps[i].c_str() );
    }
    _imp->exportLayout->addWidget(_imp->exportChoice);

    _imp->exportButton = new Button(tr("Export"),_imp->exportContainer);
    QObject::connect( _imp->exportButton,SIGNAL( clicked(bool) ),this,SLOT( onExportButtonClicked() ) );
    _imp->exportLayout->addWidget(_imp->exportButton);
    _imp->exportLayout->addStretch();
    layout->addWidget(_imp->exportContainer);
} // appendExtraGui

void
TrackerPanel::appendButtons(QHBoxLayout* buttonLayout)
{
    _imp->averageTracksButton = new Button( tr("Average tracks"),buttonLayout->parentWidget() );
    _imp->averageTracksButton->setToolTip( tr("Make a new track which is the average of the selected tracks") );
    QObject::connect( _imp->averageTracksButton, SIGNAL( clicked(bool) ), this, SLOT( onAverageTracksButtonClicked() ) );
    buttonLayout->addWidget(_imp->averageTracksButton);
}

void
TrackerPanel::initializeExtraKnobs()
{
    _imp->transformPage = Natron::createKnob<Page_Knob>(this, "Transform",1,false);

    _imp->referenceFrame = Natron::createKnob<Int_Knob>(this,"Reference frame",1,false);
    _imp->referenceFrame->setAnimationEnabled(false);
    _imp->referenceFrame->setHintToolTip("This is the frame number at which the transform will be an identity.");
    _imp->transformPage->addKnob(_imp->referenceFrame);
}

void
TrackerPanel::setIconForButton(Button_Knob* knob)
{
    const std::string name = knob->getName();

    if (name == kTrackPreviousButtonName) {
        knob->setIconFilePath(NATRON_IMAGES_PATH "back1.png");
    } else if (name == kTrackNextButtonName) {
        knob->setIconFilePath(NATRON_IMAGES_PATH "forward1.png");
    } else if (name == kTrackBackwardButtonName) {
        knob->setIconFilePath(NATRON_IMAGES_PATH "rewind.png");
    } else if (name == kTrackForwardButtonName) {
        knob->setIconFilePath(NATRON_IMAGES_PATH "play.png");
    }
}

void
TrackerPanel::onAverageTracksButtonClicked()
{
    std::list<Natron::Node*> selectedInstances;

    getSelectedInstances(&selectedInstances);
    if ( selectedInstances.empty() ) {
        Natron::warningDialog( tr("Average").toStdString(), tr("No tracks selected").toStdString() );

        return;
    }

    boost::shared_ptr<Node> newInstance = addInstanceInternal(true);
    ///give an appropriate name to the new instance
    int avgIndex = 0;
    const std::list< std::pair<boost::shared_ptr<Natron::Node>,bool > > & allInstances = getInstances();
    for (std::list< std::pair<boost::shared_ptr<Natron::Node>,bool > >::const_iterator it = allInstances.begin();
         it != allInstances.end(); ++it) {
        if ( QString( it->first->getName().c_str() ).contains("average",Qt::CaseInsensitive) ) {
            ++avgIndex;
        }
    }
    QString newName = QString("Average%1").arg(avgIndex + 1);
    newInstance->setName(newName);
    newInstance->updateEffectLabelKnob(newName);

    boost::shared_ptr<Double_Knob> newInstanceCenter = getCenterKnobForTracker( newInstance.get() );
    std::list<boost::shared_ptr<Double_Knob> > centers;
    RangeD keyframesRange;
    keyframesRange.min = INT_MAX;
    keyframesRange.max = INT_MIN;

    for (std::list<Natron::Node*>::iterator it = selectedInstances.begin(); it != selectedInstances.end(); ++it) {
        boost::shared_ptr<Double_Knob> dblKnob = getCenterKnobForTracker(*it);
        centers.push_back(dblKnob);
        double mini,maxi;
        bool hasKey = dblKnob->getFirstKeyFrameTime(0, &mini);
        if (!hasKey) {
            continue;
        }
        if (mini < keyframesRange.min) {
            keyframesRange.min = mini;
        }
        hasKey = dblKnob->getLastKeyFrameTime(0, &maxi);

        ///both dimensions must have keyframes
        assert(hasKey);
        if (maxi > keyframesRange.max) {
            keyframesRange.max = maxi;
        }
    }
    if (keyframesRange.min == INT_MIN) {
        keyframesRange.min = 0;
    }
    if (keyframesRange.max == INT_MAX) {
        keyframesRange.max = 0;
    }

    newInstanceCenter->beginChanges();
    for (double t = keyframesRange.min; t <= keyframesRange.max; ++t) {
        std::pair<double,double> average;
        average.first = 0;
        average.second = 0;
        const size_t centersNb = centers.size();
        if (centersNb) {
            for (std::list<boost::shared_ptr<Double_Knob> >::iterator it = centers.begin(); it != centers.end(); ++it) {
                double x = (*it)->getValueAtTime(t,0);
                double y = (*it)->getValueAtTime(t,1);
                average.first += x;
                average.second += y;
            }
            average.first /= centersNb;
            average.second /= centersNb;
            newInstanceCenter->setValueAtTime(t, average.first, 0);
            newInstanceCenter->setValueAtTime(t, average.second, 1);
        }
    }
    newInstanceCenter->endChanges();
} // onAverageTracksButtonClicked

void
TrackerPanel::onButtonTriggered(Button_Knob* button)
{
    std::string name = button->getName();

    ///hack the trackBackward and trackForward buttons behaviour so they appear to progress simultaneously
    if (name == kTrackBackwardButtonName) {
        trackBackward();
    } else if (name == kTrackForwardButtonName) {
        trackForward();
    } else if (name == kTrackPreviousButtonName) {
        trackPrevious();
    } else if (name == kTrackNextButtonName) {
        trackNext();
    }
}

static void
handleTrackNextAndPrevious(Button_Knob* selectedInstance,
                           SequenceTime currentFrame)
{
//        ///When a reason of eValueChangedReasonUserEdited is given, the tracker plug-in will move the timeline so just send it
//        ///upon the last track if we want to update the viewer
//        Natron::ValueChangedReasonEnum reason;
//        if (updateViewer) {
//            reason = next == selectedInstances.end() ? eValueChangedReasonNatronGuiEdited : eValueChangedReasonNatronInternalEdited;
//        } else {
//            reason = eValueChangedReasonNatronInternalEdited;
//        }
        selectedInstance->getHolder()->onKnobValueChanged_public(selectedInstance,eValueChangedReasonNatronInternalEdited,currentFrame,
                                                                 true);
}

void
TrackerPanel::onTrackingStarted()
{
    ///freeze the tracker node
    setKnobsFrozen(true);
    if (getGui()) {
        getGui()->startProgress(getMainInstance()->getLiveInstance(), tr("Tracking...").toStdString());
    }

}

void
TrackerPanel::onTrackingFinished()
{
    setKnobsFrozen(false);
    emit trackingEnded();
    if (getGui()) {
        getGui()->endProgress(getMainInstance()->getLiveInstance());
    }
}

void
TrackerPanel::onTrackingProgressUpdate(double progress)
{
    if (getGui()) {
        if (!getGui()->progressUpdate(getMainInstance()->getLiveInstance(), progress)) {
            _imp->scheduler.abortTracking();
        }
    }
}

bool
TrackerPanelPrivate::getTrackInstancesForButton(std::list<Button_Knob*>* trackButtons,const std::string& buttonName)
{
    std::list<Node*> selectedInstances;
    
    publicInterface->getSelectedInstances(&selectedInstances);
    if ( selectedInstances.empty() ) {
        Natron::warningDialog( QObject::tr("Tracker").toStdString(), QObject::tr("You must select something to track first").toStdString() );
        return false;
    }
    
    Button_Knob* prevBtn = dynamic_cast<Button_Knob*>( publicInterface->getKnobByName(buttonName).get() );
    assert(prevBtn);
    
    for (std::list<Node*>::const_iterator it = selectedInstances.begin(); it != selectedInstances.end(); ++it) {
        if ( !(*it)->getLiveInstance() ) {
            return false;
        }
        if ( (*it)->isNodeDisabled() ) {
            continue;
        }
        boost::shared_ptr<KnobI> k = (*it)->getKnobByName( prevBtn->getName() );
        Button_Knob* bKnob = dynamic_cast<Button_Knob*>( k.get() );
        assert(bKnob);
        trackButtons->push_back(bKnob);
    }
    return true;
}

bool
TrackerPanel::trackBackward()
{
    assert(QThread::currentThread() == qApp->thread());
    

    std::list<Button_Knob*> instanceButtons;
    if (!_imp->getTrackInstancesForButton(&instanceButtons, kTrackPreviousButtonName)) {
        return false;
    }
    
    int leftBound,rightBound;
    getApp()->getFrameRange(&leftBound, &rightBound);
    int end = leftBound - 1;
    int start = getApp()->getTimeLine()->currentFrame();
    
    _imp->scheduler.track(start, end, false, instanceButtons);
    
    return true;
} // trackBackward

bool
TrackerPanel::trackForward()
{
    assert(QThread::currentThread() == qApp->thread());
    
    
    std::list<Button_Knob*> instanceButtons;
    if (!_imp->getTrackInstancesForButton(&instanceButtons, kTrackNextButtonName)) {
        return false;
    }
   
    int leftBound,rightBound;
    getApp()->getFrameRange(&leftBound, &rightBound);
    boost::shared_ptr<TimeLine> timeline = getApp()->getTimeLine();
    int end = rightBound + 1;
    int start = timeline->currentFrame();
    
    _imp->scheduler.track(start, end, true, instanceButtons);
    
    return true;

} // trackForward

void
TrackerPanel::stopTracking()
{
    _imp->scheduler.abortTracking();
}

bool
TrackerPanel::trackPrevious()
{
    std::list<Node*> selectedInstances;

    getSelectedInstances(&selectedInstances);
    if ( selectedInstances.empty() ) {
        Natron::warningDialog( tr("Tracker").toStdString(), tr("You must select something to track first").toStdString() );

        return false;
    }
    std::list<Button_Knob*> instanceButtons;
    if (!_imp->getTrackInstancesForButton(&instanceButtons, kTrackPreviousButtonName)) {
        return false;
    }

    boost::shared_ptr<TimeLine> timeline = getApp()->getTimeLine();

    int start = timeline->currentFrame();
    int end = start - 1;
    
    _imp->scheduler.track(start, end, false, instanceButtons);

    return true;
}

bool
TrackerPanel::trackNext()
{
    std::list<Node*> selectedInstances;
    
    getSelectedInstances(&selectedInstances);
    if ( selectedInstances.empty() ) {
        Natron::warningDialog( tr("Tracker").toStdString(), tr("You must select something to track first").toStdString() );
        
        return false;
    }
    std::list<Button_Knob*> instanceButtons;
    if (!_imp->getTrackInstancesForButton(&instanceButtons, kTrackNextButtonName)) {
        return false;
    }
    
    boost::shared_ptr<TimeLine> timeline = getApp()->getTimeLine();
    
    int start = timeline->currentFrame();
    int end = start + 1;
    
    _imp->scheduler.track(start, end, true, instanceButtons);
    
    return true;
}

void
TrackerPanel::clearAllAnimationForSelection()
{
    std::list<Node*> selectedInstances;

    getSelectedInstances(&selectedInstances);
    for (std::list<Node*>::const_iterator it = selectedInstances.begin(); it != selectedInstances.end(); ++it) {
        const std::vector<boost::shared_ptr<KnobI> > & knobs = (*it)->getKnobs();
        for (U32 i = 0; i < knobs.size(); ++i) {
            for (int dim = 0; dim < knobs[i]->getDimension(); ++dim) {
                knobs[i]->removeAnimation(dim);
            }
        }
    }
}

void
TrackerPanel::clearBackwardAnimationForSelection()
{
    int time = getApp()->getTimeLine()->currentFrame();
    std::list<Node*> selectedInstances;

    getSelectedInstances(&selectedInstances);
    for (std::list<Node*>::const_iterator it = selectedInstances.begin(); it != selectedInstances.end(); ++it) {
        const std::vector<boost::shared_ptr<KnobI> > & knobs = (*it)->getKnobs();
        for (U32 i = 0; i < knobs.size(); ++i) {
            for (int dim = 0; dim < knobs[i]->getDimension(); ++dim) {
                knobs[i]->deleteAnimationBeforeTime(time,dim,Natron::eValueChangedReasonPluginEdited);
            }
        }
    }
}

void
TrackerPanel::clearForwardAnimationForSelection()
{
    int time = getApp()->getTimeLine()->currentFrame();
    std::list<Node*> selectedInstances;

    getSelectedInstances(&selectedInstances);
    for (std::list<Node*>::const_iterator it = selectedInstances.begin(); it != selectedInstances.end(); ++it) {
        const std::vector<boost::shared_ptr<KnobI> > & knobs = (*it)->getKnobs();
        for (U32 i = 0; i < knobs.size(); ++i) {
            for (int dim = 0; dim < knobs[i]->getDimension(); ++dim) {
                knobs[i]->deleteAnimationAfterTime(time,dim,Natron::eValueChangedReasonPluginEdited);
            }
        }
    }
}

void
TrackerPanel::setUpdateViewerOnTracking(bool update)
{
    QMutexLocker k(&_imp->updateViewerMutex);
    _imp->updateViewerOnTrackingEnabled = update;
}

bool
TrackerPanel::isUpdateViewerOnTrackingEnabled() const
{
    QMutexLocker k(&_imp->updateViewerMutex);
    return _imp->updateViewerOnTrackingEnabled;
}

void
TrackerPanel::onExportButtonClicked()
{
    int index = _imp->exportChoice->activeIndex();
    std::list<Node*> selection;

    getSelectedInstances(&selection);
    ///This is the full list, decomment when everything will be possible to do
//    switch (index) {
//        case 0:
//            _imp->createCornerPinFromSelection(selection, true, false);
//            break;
//        case 1:
//            _imp->createCornerPinFromSelection(selection, true, true);
//            break;
//        case 2:
//            _imp->createCornerPinFromSelection(selection, false, false);
//            break;
//        case 3:
//            _imp->createCornerPinFromSelection(selection, false, true);
//            break;
//        case 4:
//            _imp->createTransformFromSelection(selection, true, eExportTransformTypeStabilize);
//            break;
//        case 5:
//            _imp->createTransformFromSelection(selection, true, eExportTransformTypeMatchMove);
//            break;
//        case 6:
//            _imp->createTransformFromSelection(selection, false, eExportTransformTypeStabilize);
//            break;
//        case 7:
//            _imp->createTransformFromSelection(selection, false, eExportTransformTypeMatchMove);
//            break;
//        default:
//            break;
    //    }
    switch (index) {
        case 0:
            _imp->createCornerPinFromSelection(selection, true, false,false);
            break;
        case 1:
            _imp->createCornerPinFromSelection(selection, true, false,true);
            break;
        case 2:
            _imp->createCornerPinFromSelection(selection, false, false,false);
            break;
        case 3:
            _imp->createCornerPinFromSelection(selection, false, false,true);
            break;
        case 4:
            _imp->createCornerPinFromSelection(selection, false, true,false);
            break;
        default:
            break;
    }
}

void
TrackerPanelPrivate::createTransformFromSelection(const std::list<Node*> & /*selection*/,
                                                  bool /*linked*/,
                                                  ExportTransformTypeEnum /*type*/)
{
}

namespace  {

boost::shared_ptr<Double_Knob>
getCornerPinPoint(Natron::Node* node,
                  bool isFrom,
                  int index)
{
    assert(0 <= index && index < 4);
    QString name = isFrom ? QString("from%1").arg(index + 1) : QString("to%1").arg(index + 1);
    boost::shared_ptr<KnobI> knob = node->getKnobByName( name.toStdString() );
    assert(knob);
    boost::shared_ptr<Double_Knob>  ret = boost::dynamic_pointer_cast<Double_Knob>(knob);
    assert(ret);
    return ret;
}
}

void
TrackerPanelPrivate::createCornerPinFromSelection(const std::list<Node*> & selection,
                                                  bool linked,
                                                  bool useTransformRefFrame,
                                                  bool invert)
{
    if ( (selection.size() > 4) || selection.empty() ) {
        Natron::errorDialog( QObject::tr("Export").toStdString(),
                             QObject::tr("Export to corner pin needs between 1 and 4 selected tracks.").toStdString() );

        return;
    }

    boost::shared_ptr<Double_Knob> centers[4];
    int i = 0;
    for (std::list<Node*>::const_iterator it = selection.begin(); it != selection.end(); ++it,++i) {
        centers[i] = getCenterKnobForTracker(*it);
        assert(centers[i]);
    }
    GuiAppInstance* app = publicInterface->getGui()->getApp();
    boost::shared_ptr<Natron::Node> cornerPin = app->createNode( CreateNodeArgs("CornerPinOFX  [Transform]",
                                                                                "",
                                                                                -1, -1, -1,
                                                                                false, //< don't autoconnect
                                                                                INT_MIN,
                                                                                INT_MIN,
                                                                                true,
                                                                                true,
                                                                                QString(),
                                                                                CreateNodeArgs::DefaultValuesList()) );
    if (!cornerPin) {
        return;
    }

    ///Move the node on the right of the tracker node
    boost::shared_ptr<NodeGui> cornerPinGui = app->getNodeGui(cornerPin);
    assert(cornerPinGui);

    boost::shared_ptr<NodeGui> mainInstanceGui = app->getNodeGui( publicInterface->getMainInstance() );
    assert(mainInstanceGui);

    QPointF mainInstancePos = mainInstanceGui->scenePos();
    mainInstancePos = cornerPinGui->mapToParent( cornerPinGui->mapFromScene(mainInstancePos) );
    cornerPinGui->refreshPosition( mainInstancePos.x() + mainInstanceGui->getSize().width() * 2, mainInstancePos.y() );

    boost::shared_ptr<Double_Knob> toPoints[4];
    boost::shared_ptr<Double_Knob>  fromPoints[4];
    
    int timeForFromPoints = useTransformRefFrame ? referenceFrame->getValue() : app->getTimeLine()->currentFrame();

    for (unsigned int i = 0; i < selection.size(); ++i) {
        fromPoints[i] = getCornerPinPoint(cornerPin.get(), true, i);
        assert(fromPoints[i] && centers[i]);
        for (int j = 0; j < fromPoints[i]->getDimension(); ++j) {
            fromPoints[i]->setValue(centers[i]->getValueAtTime(timeForFromPoints,j), j);
        }

        toPoints[i] = getCornerPinPoint(cornerPin.get(), false, i);
        assert(toPoints[i]);
        if (!linked) {
            toPoints[i]->cloneAndUpdateGui(centers[i].get());
        } else {
            dynamic_cast<KnobI*>(toPoints[i].get())->slaveTo(0, centers[i], 0);
            dynamic_cast<KnobI*>(toPoints[i].get())->slaveTo(1, centers[i], 1);
        }
    }

    ///Disable all non used points
    for (unsigned int i = selection.size(); i < 4; ++i) {
        QString enableName = QString("enable%1").arg(i + 1);
        boost::shared_ptr<KnobI> knob = cornerPin->getKnobByName( enableName.toStdString() );
        assert(knob);
        Bool_Knob* enableKnob = dynamic_cast<Bool_Knob*>( knob.get() );
        assert(enableKnob);
        enableKnob->setValue(false, 0);
    }
    
    if (invert) {
        boost::shared_ptr<KnobI> invertKnob = cornerPin->getKnobByName(kTrackInvertName);
        assert(invertKnob);
        Bool_Knob* isBool = dynamic_cast<Bool_Knob*>(invertKnob.get());
        assert(isBool);
        isBool->setValue(true, 0);
    }
    
} // createCornerPinFromSelection

void
TrackerPanel::showMenuForInstance(Natron::Node* instance)
{
    QMenu menu( getGui() );

    menu.setFont( QFont(appFont,appFontSize) );

    QAction* copyTrackAnimation = new QAction(tr("Copy track animation"),&menu);
    menu.addAction(copyTrackAnimation);

    QAction* ret = menu.exec( QCursor::pos() );
    if (ret == copyTrackAnimation) {
        boost::shared_ptr<Double_Knob> centerKnob = getCenterKnobForTracker(instance);
        assert(centerKnob);
        centerKnob->copyAnimationToClipboard();
    }
}


struct TrackArgs
{
    int start,end;
    bool forward;
    std::list<Button_Knob*> instances;
};

struct TrackSchedulerPrivate
{
    const TrackerPanel* panel;
    
    QMutex argsMutex;
    TrackArgs curArgs,requestedArgs;
    
    mutable QMutex mustQuitMutex;
    bool mustQuit;
    QWaitCondition mustQuitCond;
    
    mutable QMutex abortRequestedMutex;
    int abortRequested;
    QWaitCondition abortRequestedCond;
    
    QMutex startRequesstMutex;
    int startRequests;
    QWaitCondition startRequestsCond;
    
    mutable QMutex isWorkingMutex;
    bool isWorking;
    
    
    TrackSchedulerPrivate(const TrackerPanel* panel)
    : panel(panel)
    , argsMutex()
    , curArgs()
    , requestedArgs()
    , mustQuitMutex()
    , mustQuit(false)
    , mustQuitCond()
    , abortRequestedMutex()
    , abortRequested(0)
    , abortRequestedCond()
    , startRequesstMutex()
    , startRequests(0)
    , startRequestsCond()
    , isWorkingMutex()
    , isWorking(false)
    {
        
    }
    
    bool checkForExit()
    {
        QMutexLocker k(&mustQuitMutex);
        if (mustQuit) {
            mustQuit = false;
            mustQuitCond.wakeAll();
            return true;
        }
        return false;
    }
    
};


TrackScheduler::TrackScheduler(const TrackerPanel* panel)
: QThread()
, _imp(new TrackSchedulerPrivate(panel))
{
    setObjectName("TrackScheduler");
}

TrackScheduler::~TrackScheduler()
{
    
}

bool
TrackScheduler::isWorking() const
{
    QMutexLocker k(&_imp->isWorkingMutex);
    return _imp->isWorking;
}

void
TrackScheduler::run()
{
    for (;;) {
        
        ///Check for exit of the thread
        if (_imp->checkForExit()) {
            return;
        }
        
        ///Flag that we're working
        {
            QMutexLocker k(&_imp->isWorkingMutex);
            _imp->isWorking = true;
        }
        
        ///Copy the requested args to the args used for processing
        {
            QMutexLocker k(&_imp->argsMutex);
            _imp->curArgs = _imp->requestedArgs;
        }
        
        boost::shared_ptr<TimeLine> timeline = _imp->panel->getApp()->getTimeLine();

        int end = _imp->curArgs.end;
        int start = _imp->curArgs.start;
        int cur = start;
        
        int framesCount = _imp->curArgs.forward ? (end - start) : (start - end);
        
        bool reportProgress = _imp->curArgs.instances.size() > 1 || framesCount > 1;
        if (reportProgress) {
            emit trackingStarted();
        }
        
        while (cur != end) {
            
            
            ///Launch parallel thread for each track using the global thread pool
            QtConcurrent::map(_imp->curArgs.instances,
                              boost::bind(&handleTrackNextAndPrevious,
                                          _1,
                                          cur)).waitForFinished();
            
            
            double progress;
            if (_imp->curArgs.forward) {
                ++cur;
                progress = (double)(cur - start) / framesCount;
            } else {
                --cur;
                progress = (double)(start - cur) / framesCount;
            }
            
            ///Ok all tracks are finished now for this frame, refresh viewer if needed
            bool updateViewer = _imp->panel->isUpdateViewerOnTrackingEnabled();
            if (updateViewer) {
                timeline->seekFrame(cur, true, 0, Natron::eTimelineChangeReasonPlaybackSeek);
            }

            if (reportProgress) {
                ///Notify we progressed of 1 frame
                emit progressUpdate(progress);
            }
            
            ///Check for abortion
            {
                QMutexLocker k(&_imp->abortRequestedMutex);
                if (_imp->abortRequested > 0) {
                    _imp->abortRequested = 0;
                    _imp->abortRequestedCond.wakeAll();
                    break;
                }
            }

        }
        
        if (reportProgress) {
            emit trackingFinished();
        }
        
        ///Flag that we're no longer working
        {
            QMutexLocker k(&_imp->isWorkingMutex);
            _imp->isWorking = false;
        }
        
        ///Make sure we really reset the abort flag
        {
            QMutexLocker k(&_imp->abortRequestedMutex);
            if (_imp->abortRequested > 0) {
                _imp->abortRequested = 0;
                
            }
        }
        
        ///Sleep or restart if we've requests in the queue
        {
            QMutexLocker k(&_imp->startRequesstMutex);
            while (_imp->startRequests <= 0) {
                _imp->startRequestsCond.wait(&_imp->startRequesstMutex);
            }
            _imp->startRequests = 0;
        }
        
    }
}

void
TrackScheduler::track(int startingFrame,int end,bool forward, const std::list<Button_Knob*> & selectedInstances)
{
    if ((forward && startingFrame >= end) || (!forward && startingFrame <= end)) {
        emit trackingFinished();
        return;
    }
    {
        QMutexLocker k(&_imp->argsMutex);
        _imp->requestedArgs.start = startingFrame;
        _imp->requestedArgs.end = end;
        _imp->requestedArgs.forward = forward;
        _imp->requestedArgs.instances = selectedInstances;
    }
    if (isRunning()) {
        QMutexLocker k(&_imp->startRequesstMutex);
        ++_imp->startRequests;
        _imp->startRequestsCond.wakeAll();
    } else {
        start();
    }
}


void TrackScheduler::abortTracking()
{
    if (!isRunning() || !isWorking()) {
        return;
    }
    
    
    {
        QMutexLocker k(&_imp->abortRequestedMutex);
        ++_imp->abortRequested;
        _imp->abortRequestedCond.wakeAll();
    }
    
}

void
TrackScheduler::quitThread()
{
    if (!isRunning()) {
        return;
    }
    
    abortTracking();
    
    {
        QMutexLocker k(&_imp->mustQuitMutex);
        _imp->mustQuit = true;

        {
            QMutexLocker k(&_imp->startRequesstMutex);
            ++_imp->startRequests;
            _imp->startRequestsCond.wakeAll();
        }
        
        while (_imp->mustQuit) {
            _imp->mustQuitCond.wait(&_imp->mustQuitMutex);
        }

    }

    
    wait();
    
}
