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

#include "Node.h"
#include "ofxNatron.h"

#include <limits>

#include <QtCore/QDebug>
#include <QtCore/QReadWriteLock>
#include <QtCore/QCoreApplication>
#include <QtCore/QWaitCondition>
#include <boost/bind.hpp>

#include <ofxNatron.h>

#include "Engine/Hash64.h"
#include "Engine/Format.h"
#include "Engine/ViewerInstance.h"
#include "Engine/OfxHost.h"
#include "Engine/Knob.h"
#include "Engine/OfxEffectInstance.h"
#include "Engine/TimeLine.h"
#include "Engine/Lut.h"
#include "Engine/Image.h"
#include "Engine/Project.h"
#include "Engine/EffectInstance.h"
#include "Engine/Log.h"
#include "Engine/NodeSerialization.h"
#include "Engine/Plugin.h"
#include "Engine/AppInstance.h"
#include "Engine/AppManager.h"
#include "Engine/LibraryBinary.h"
#include "Engine/KnobTypes.h"
#include "Engine/ImageParams.h"
#include "Engine/ThreadStorage.h"
#include "Engine/RotoContext.h"
#include "Engine/Timer.h"
#include "Engine/Settings.h"
#include "Engine/NodeGuiI.h"

///The flickering of edges/nodes in the nodegraph will be refreshed
///at most every...
#define NATRON_RENDER_GRAPHS_HINTS_REFRESH_RATE_SECONDS 0.5

using namespace Natron;
using std::make_pair;
using std::cout; using std::endl;
using boost::shared_ptr;

namespace { // protect local classes in anonymous namespace
    /*The output node was connected from inputNumber to this...*/
    typedef std::map<Node*,int > DeactivatedState;
    typedef std::list<Node::KnobLink> KnobLinkList;
    typedef std::vector<boost::shared_ptr<Node> > InputsV;
    
    enum InputActionEnum
    {
        eInputActionConnect,
        eInputActionDisconnect,
        eInputActionReplace
    };
    
    struct ConnectInputAction
    {
        boost::shared_ptr<Natron::Node> node;
        InputActionEnum type;
        int inputNb;
        
        ConnectInputAction(const boost::shared_ptr<Natron::Node>& input,InputActionEnum type,int inputNb)
        : node(input)
        , type(type)
        , inputNb(inputNb)
        {
        }
    };
    
}



struct Node::Implementation
{
    Implementation(AppInstance* app_,
                   Natron::Plugin* plugin_)
    : app(app_)
    , knobsInitialized(false)
    , inputsInitialized(false)
    , outputsMutex()
    , outputs()
    , inputsMutex()
    , inputs()
    , liveInstance(0)
    , effectCreated(false)
    , inputsComponents()
    , outputComponents()
    , inputLabels()
    , name()
    , deactivatedState()
    , activatedMutex()
    , activated(true)
    , plugin(plugin_)
    , computingPreview(false)
    , computingPreviewMutex()
    , pluginInstanceMemoryUsed(0)
    , memoryUsedMutex()
    , mustQuitPreview(false)
    , mustQuitPreviewMutex()
    , mustQuitPreviewCond()
    , knobsAge(0)
    , knobsAgeMutex()
    , masterNodeMutex()
    , masterNode()
    , nodeLinks()
    , enableMaskKnob()
    , maskChannelKnob()
    , nodeSettingsPage()
    , nodeLabelKnob()
    , previewEnabledKnob()
    , disableNodeKnob()
    , infoPage()
    , infoDisclaimer()
    , inputFormats()
    , outputFormat()
    , refreshInfoButton()
    , useFullScaleImagesWhenRenderScaleUnsupported()
    , forceCaching()
    , rotoContext()
    , imagesBeingRenderedMutex()
    , imageBeingRenderedCond()
    , imagesBeingRendered()
    , supportedDepths()
    , isMultiInstance(false)
    , multiInstanceParent(NULL)
    , multiInstanceParentName()
    , duringInputChangedAction(false)
    , keyframesDisplayedOnTimeline(false)
    , timersMutex()
    , lastRenderStartedSlotCallTime()
    , lastInputNRenderStartedSlotCallTime()
    , connectionQueue()
    , connectionQueueMutex()
    , nodeIsDequeuing(false)
    , nodeIsDequeuingMutex()
    , nodeIsDequeuingCond()
    , nodeIsRendering(0)
    , nodeIsRenderingMutex()
    , mustQuitProcessing(false)
    , mustQuitProcessingMutex()
    , persistentMessage()
    , persistentMessageType(0)
    , persistentMessageMutex()
    , guiPointer(0)
    {
        ///Initialize timers
        gettimeofday(&lastRenderStartedSlotCallTime, 0);
        gettimeofday(&lastInputNRenderStartedSlotCallTime, 0);
    }
    
    void abortPreview();
    
    bool checkForExitPreview();
    
    void setComputingPreview(bool v) {
        QMutexLocker l(&computingPreviewMutex);
        computingPreview = v;
    }
    
    AppInstance* app; // pointer to the app: needed to access the application's default-project's format
    bool knobsInitialized;
    bool inputsInitialized;
    mutable QMutex outputsMutex;
    std::list<Node*> outputs;
    
    mutable QMutex inputsMutex; //< protects guiInputs so the serialization thread can access them
    
    ///The  inputs are the one used by the GUI whenever the user make changes.
    ///Finally we use only one set of inputs protected by a mutex. This has the drawback that the
    ///inputs connections might change throughout the render of a frame and the plug-in might then have
    ///different results when calling stuff on clips (e.g: getRegionOfDefinition or getComponents).
    ///I couldn't figure out a clean way to have the inputs stable throughout a render, thread-storage is not enough:
    ///image effect suite functions that access to clips can be called from other threads than just the render-thread:
    ///they can be accessed from the multi-thread suite. The problem is we have no way to set thread-local data to multi
    ///thread suite threads because we don't know at all which node is using it.
    InputsV inputs;
    
    //to the inputs in a thread-safe manner.
    Natron::EffectInstance*  liveInstance; //< the effect hosted by this node
    bool effectCreated;
    
    ///These two are also protected by inputsMutex
    std::vector< std::list<Natron::ImageComponentsEnum> > inputsComponents;
    std::list<Natron::ImageComponentsEnum> outputComponents;
    mutable QMutex nameMutex;
    std::vector<std::string> inputLabels; // inputs name
    std::string name; //node name set by the user
    DeactivatedState deactivatedState;
    mutable QMutex activatedMutex;
    bool activated;
    Natron::Plugin* plugin; //< the plugin which stores the function to instantiate the effect
    bool computingPreview;
    mutable QMutex computingPreviewMutex;
    size_t pluginInstanceMemoryUsed; //< global count on all EffectInstance's of the memory they use.
    QMutex memoryUsedMutex; //< protects _pluginInstanceMemoryUsed
    bool mustQuitPreview;
    QMutex mustQuitPreviewMutex;
    QWaitCondition mustQuitPreviewCond;
    QMutex renderInstancesSharedMutex; //< see eRenderSafetyInstanceSafe in EffectInstance::renderRoI
    //only 1 clone can render at any time
    
    U64 knobsAge; //< the age of the knobs in this effect. It gets incremented every times the liveInstance has its evaluate() function called.
    mutable QReadWriteLock knobsAgeMutex; //< protects knobsAge and hash
    Hash64 hash; //< recomputed everytime knobsAge is changed.
    mutable QMutex masterNodeMutex; //< protects masterNode and nodeLinks
    boost::shared_ptr<Node> masterNode; //< this points to the master when the node is a clone
    KnobLinkList nodeLinks; //< these point to the parents of the params links
    
    ///For each mask, the input number and the knob
    std::map<int,boost::shared_ptr<Bool_Knob> > enableMaskKnob;
    std::map<int,boost::shared_ptr<Choice_Knob> > maskChannelKnob;
    boost::shared_ptr<Page_Knob> nodeSettingsPage;
    boost::shared_ptr<String_Knob> nodeLabelKnob;
    boost::shared_ptr<Bool_Knob> previewEnabledKnob;
    boost::shared_ptr<Bool_Knob> disableNodeKnob;
    
    boost::shared_ptr<Page_Knob> infoPage;
    boost::shared_ptr<String_Knob> infoDisclaimer;
    std::vector< boost::shared_ptr<String_Knob> > inputFormats;
    boost::shared_ptr<String_Knob> outputFormat;
    boost::shared_ptr<Button_Knob> refreshInfoButton;
    
    boost::shared_ptr<Bool_Knob> useFullScaleImagesWhenRenderScaleUnsupported;
    boost::shared_ptr<Bool_Knob> forceCaching;
    
    boost::shared_ptr<RotoContext> rotoContext; //< valid when the node has a rotoscoping context (i.e: paint context)
    
    mutable QMutex imagesBeingRenderedMutex;
    QWaitCondition imageBeingRenderedCond;
    std::list< boost::shared_ptr<Image> > imagesBeingRendered; ///< a list of all the images being rendered simultaneously
    
    std::list <Natron::ImageBitDepthEnum> supportedDepths;
    
    ///True when several effect instances are represented under the same node.
    bool isMultiInstance;
    Natron::Node* multiInstanceParent;
    
    ///the name of the parent at the time this node was created
    std::string multiInstanceParentName;
    bool duringInputChangedAction; //< true if we're during onInputChanged(...). MT-safe since only modified by the main thread
    bool keyframesDisplayedOnTimeline;
    
    ///This is to avoid the slots connected to the main-thread to be called too much
    QMutex timersMutex; //< protects lastRenderStartedSlotCallTime & lastInputNRenderStartedSlotCallTime
    timeval lastRenderStartedSlotCallTime;
    timeval lastInputNRenderStartedSlotCallTime;
    
    ///Used when the node is rendering and dequeued when it is done rendering
    std::list<ConnectInputAction> connectionQueue;
    QMutex connectionQueueMutex;
    
    ///True when the node is dequeuing the connectionQueue and no render should be started 'til it is empty
    bool nodeIsDequeuing;
    QMutex nodeIsDequeuingMutex;
    QWaitCondition nodeIsDequeuingCond;
    
    ///Counter counting how many parallel renders are active on the node
    int nodeIsRendering;
    mutable QMutex nodeIsRenderingMutex;
    
    bool mustQuitProcessing;
    mutable QMutex mustQuitProcessingMutex;
    
    QString persistentMessage;
    int persistentMessageType;
    mutable QMutex persistentMessageMutex;
    
    NodeGuiI* guiPointer;
};

/**
 *@brief Actually converting to ARGB... but it is called BGRA by
 the texture format GL_UNSIGNED_INT_8_8_8_8_REV
 **/
static unsigned int toBGRA(unsigned char r, unsigned char g, unsigned char b, unsigned char a) WARN_UNUSED_RETURN;
unsigned int
toBGRA(unsigned char r,
       unsigned char g,
       unsigned char b,
       unsigned char a)
{
    return (a << 24) | (r << 16) | (g << 8) | b;
}

Node::Node(AppInstance* app,
           Natron::Plugin* plugin)
: QObject()
, _imp( new Implementation(app,plugin) )
{
    QObject::connect( this, SIGNAL( pluginMemoryUsageChanged(qint64) ), appPTR, SLOT( onNodeMemoryRegistered(qint64) ) );
    QObject::connect(this, SIGNAL(mustDequeueActions()), this, SLOT(dequeueActions()));
}

void
Node::createRotoContextConditionnally()
{
    assert(!_imp->rotoContext);
    assert(_imp->liveInstance);
    ///Initialize the roto context if any
    if ( isRotoNode() ) {
        _imp->rotoContext.reset( new RotoContext(this) );
        _imp->rotoContext->createBaseLayer();
    }
}

const Natron::Plugin*
Node::getPlugin() const
{
    return _imp->plugin;
}

void
Node::load(const std::string & pluginID,
           const std::string & parentMultiInstanceName,
           int childIndex,
           const boost::shared_ptr<Natron::Node> & thisShared,
           const NodeSerialization & serialization,
           bool dontLoadName,
           const QString& fixedName,
           const CreateNodeArgs::DefaultValuesList& paramValues)
{
    ///Called from the main thread. MT-safe
    assert( QThread::currentThread() == qApp->thread() );
    
    ///cannot load twice
    assert(!_imp->liveInstance);
    
    
    bool nameSet = false;
    bool isMultiInstanceChild = false;
    if ( !parentMultiInstanceName.empty() ) {
        _imp->multiInstanceParentName = parentMultiInstanceName;
        
        ///Fetch the parent pointer ONLY when not loading (otherwise parent node might still node be created
        ///at that time)
        if ( serialization.isNull() && fixedName.isEmpty() ) {
            setName( QString( parentMultiInstanceName.c_str() ) + '_' + QString::number(childIndex) );
            nameSet = true;
        }
        isMultiInstanceChild = true;
        _imp->isMultiInstance = false;
    }
    
    if (!serialization.isNull() && !dontLoadName && !nameSet && fixedName.isEmpty()) {
        setName( serialization.getPluginLabel().c_str() );
        nameSet = true;
    }
    
    if (serialization.isNull() && !parentMultiInstanceName.empty()) {
        fetchParentMultiInstancePointer();
    }
    
    
    int renderScaleSupportPreference = appPTR->getCurrentSettings()->getRenderScaleSupportPreference(pluginID);

    LibraryBinary* binary = _imp->plugin->getLibraryBinary();
    std::pair<bool,EffectBuilder> func;
    if (binary) {
        func = binary->findFunction<EffectBuilder>("BuildEffect");
    }
    bool isFileDialogPreviewReader = fixedName.contains("Natron_File_Dialog_Preview_Provider_Reader");
    if (func.first) {
        _imp->liveInstance = func.second(thisShared);
        assert(_imp->liveInstance);
        _imp->liveInstance->initializeData();
        
        createRotoContextConditionnally();
        initializeInputs();
        initializeKnobs(serialization,renderScaleSupportPreference);
        if (!paramValues.empty()) {
            setValuesFromSerialization(paramValues);
        }
        
        
        std::string images;
        if (_imp->liveInstance->isReader() && serialization.isNull() && paramValues.empty() && !isFileDialogPreviewReader) {
            images = getApp()->openImageFileDialog();
        } else if (_imp->liveInstance->isWriter() && serialization.isNull() && paramValues.empty() && !isFileDialogPreviewReader) {
            images = getApp()->saveImageFileDialog();
        }
        if (!images.empty()) {
            boost::shared_ptr<KnobSerialization> defaultFile = createDefaultValueForParam(kOfxImageEffectFileParamName, images);
            CreateNodeArgs::DefaultValuesList list;
            list.push_back(defaultFile);
            setValuesFromSerialization(list);
        }
    } else { //ofx plugin
                
        _imp->liveInstance = appPTR->createOFXEffect(pluginID,thisShared,&serialization,paramValues,!isFileDialogPreviewReader,renderScaleSupportPreference == 1);
        assert(_imp->liveInstance);
        _imp->liveInstance->initializeOverlayInteract();
    }
    
    _imp->liveInstance->addSupportedBitDepth(&_imp->supportedDepths);
    
    if ( _imp->supportedDepths.empty() ) {
        //From the spec:
        //The default for a plugin is to have none set, the plugin \em must define at least one in its describe action.
        throw std::runtime_error("Plug-in does not support 8bits, 16bits or 32bits floating point image processing.");
    }
    
    
    ///Special case for trackers: set as multi instance
    if ( isTrackerNode() ) {
        _imp->isMultiInstance = true;
        ///declare knob that are instance specific
        boost::shared_ptr<KnobI> subLabelKnob = getKnobByName(kNatronOfxParamStringSublabelName);
        if (subLabelKnob) {
            subLabelKnob->setAsInstanceSpecific();
        }
        
        boost::shared_ptr<KnobI> centerKnob = getKnobByName("center");
        if (centerKnob) {
            centerKnob->setAsInstanceSpecific();
        }
        
        boost::shared_ptr<KnobI> offsetKnob = getKnobByName("offset");
        if (offsetKnob) {
            offsetKnob->setAsInstanceSpecific();
        }
    }
    
    if (!nameSet) {
        if (fixedName.isEmpty()) {
            getApp()->getProject()->initNodeCountersAndSetName(this);
        } else {
            setName(fixedName);
        }
        if (!isMultiInstanceChild && _imp->isMultiInstance) {
            updateEffectLabelKnob( getName().c_str() );
        }
    }
    if ( isMultiInstanceChild && serialization.isNull() ) {
        assert(nameSet);
        updateEffectLabelKnob( QString( parentMultiInstanceName.c_str() ) + '_' + QString::number(childIndex) );
    }
    
    computeHash();
    assert(_imp->liveInstance);
} // load

void
Node::fetchParentMultiInstancePointer()
{
    std::vector<boost::shared_ptr<Node> > nodes = getApp()->getProject()->getCurrentNodes();
    
    for (U32 i = 0; i < nodes.size(); ++i) {
        if (nodes[i]->getName() == _imp->multiInstanceParentName) {
            ///no need to store the boost pointer because the main instance lives the same time
            ///as the child
            _imp->multiInstanceParent = nodes[i].get();
            QObject::connect(nodes[i].get(), SIGNAL(inputChanged(int)), this, SLOT(onParentMultiInstanceInputChanged(int)));
            break;
        }
    }
}

Natron::Node*
Node::getParentMultiInstance() const
{
    return _imp->multiInstanceParent;
}

bool
Node::isMultiInstance() const
{
    return _imp->isMultiInstance;
}

///Accessed by the serialization thread, but mt safe since never changed
std::string
Node::getParentMultiInstanceName() const
{
    return _imp->multiInstanceParentName;
}

U64
Node::getHashValue() const
{
    QReadLocker l(&_imp->knobsAgeMutex);
    
    return _imp->hash.value();
}

void
Node::computeHash()
{
    ///Always called in the main thread
    assert( QThread::currentThread() == qApp->thread() );
    if (!_imp->inputsInitialized) {
        qDebug() << "Node::computeHash(): inputs not initialized";
    }
    
    {
        QWriteLocker l(&_imp->knobsAgeMutex);
        
        ///reset the hash value
        _imp->hash.reset();
        
        ///append the effect's own age
        _imp->hash.append(_imp->knobsAge);
        
        ///append all inputs hash
        {
            ViewerInstance* isViewer = dynamic_cast<ViewerInstance*>(_imp->liveInstance);
            
            if (isViewer) {
                int activeInput[2];
                isViewer->getActiveInputs(activeInput[0], activeInput[1]);
                
                for (int i = 0; i < 2; ++i) {
                    if ( (activeInput[i] >= 0) && _imp->inputs[activeInput[i]] ) {
                        _imp->hash.append( _imp->inputs[activeInput[i]]->getHashValue() );
                    }
                }
            } else {
                for (U32 i = 0; i < _imp->inputs.size(); ++i) {
                    if (_imp->inputs[i]) {
                        ///Add the index of the input to its hash.
                        ///Explanation: if we didn't add this, just switching inputs would produce a similar
                        ///hash.
                        _imp->hash.append(_imp->inputs[i]->getHashValue() + i);
                    }
                }
            }
        }
        
        ///Also append the effect's label to distinguish 2 instances with the same parameters
        ::Hash64_appendQString( &_imp->hash, QString( getName().c_str() ) );
        
        
        ///Also append the project's creation time in the hash because 2 projects openend concurrently
        ///could reproduce the same (especially simple graphs like Viewer-Reader)
        qint64 creationTime =  getApp()->getProject()->getProjectCreationTime();
        _imp->hash.append(creationTime);
        
        _imp->hash.computeHash();
    }
    
    ///call it on all the outputs
    for (std::list<Node*>::iterator it = _imp->outputs.begin(); it != _imp->outputs.end(); ++it) {
        assert(*it);
        (*it)->computeHash();
    }
    _imp->liveInstance->onNodeHashChanged(getHashValue());
    
} // computeHash

void
Node::setValuesFromSerialization(const std::list<boost::shared_ptr<KnobSerialization> >& paramValues)
{
    
    assert( QThread::currentThread() == qApp->thread() );
    assert(_imp->knobsInitialized);
    
    const std::vector< boost::shared_ptr<KnobI> > & nodeKnobs = getKnobs();
    ///for all knobs of the node
    for (U32 j = 0; j < nodeKnobs.size(); ++j) {
        ///try to find a serialized value for this knob
        for (std::list<boost::shared_ptr<KnobSerialization> >::const_iterator it = paramValues.begin(); it != paramValues.end(); ++it) {
            if ( (*it)->getName() == nodeKnobs[j]->getName() ) {
                boost::shared_ptr<KnobI> serializedKnob = (*it)->getKnob();
                nodeKnobs[j]->clone(serializedKnob);
                break;
            }
        }
        
    }
}

void
Node::loadKnobs(const NodeSerialization & serialization,bool updateKnobGui)
{
    ///Only called from the main thread
    assert( QThread::currentThread() == qApp->thread() );
    assert(_imp->knobsInitialized);
    
    const std::vector< boost::shared_ptr<KnobI> > & nodeKnobs = getKnobs();
    ///for all knobs of the node
    for (U32 j = 0; j < nodeKnobs.size(); ++j) {
        loadKnob(nodeKnobs[j], serialization,updateKnobGui);
    }
    ///now restore the roto context if the node has a roto context
    if (serialization.hasRotoContext() && _imp->rotoContext) {
        _imp->rotoContext->load( serialization.getRotoContext() );
    }
    
    setKnobsAge( serialization.getKnobsAge() );
}

void
Node::loadKnob(const boost::shared_ptr<KnobI> & knob,
               const NodeSerialization & serialization,bool updateKnobGui)
{
    const NodeSerialization::KnobValues & knobsValues = serialization.getKnobsValues();
    
    ///try to find a serialized value for this knob
    for (NodeSerialization::KnobValues::const_iterator it = knobsValues.begin(); it != knobsValues.end(); ++it) {
        if ( (*it)->getName() == knob->getName() ) {
            
            // don't load the value if the Knob is not persistant! (it is just the default value in this case)
            ///EDIT: Allow non persistent params to be loaded if we found a valid serialization for them
            //if ( knob->getIsPersistant() ) {
            boost::shared_ptr<KnobI> serializedKnob = (*it)->getKnob();
            
            Choice_Knob* isChoice = dynamic_cast<Choice_Knob*>(knob.get());
            if (isChoice) {
                const TypeExtraData* extraData = (*it)->getExtraData();
                const ChoiceExtraData* choiceData = dynamic_cast<const ChoiceExtraData*>(extraData);
                assert(choiceData);
                
                Choice_Knob* choiceSerialized = dynamic_cast<Choice_Knob*>(serializedKnob.get());
                assert(choiceSerialized);
                isChoice->choiceRestoration(choiceSerialized, choiceData);
            } else {
                if (updateKnobGui) {
                    knob->cloneAndUpdateGui(serializedKnob.get());
                } else {
                    knob->clone(serializedKnob);
                }
                knob->setSecret( serializedKnob->getIsSecret() );
                if ( knob->getDimension() == serializedKnob->getDimension() ) {
                    for (int i = 0; i < knob->getDimension(); ++i) {
                        knob->setEnabled( i, serializedKnob->isEnabled(i) );
                    }
                }
            }
            
            if (knob->getName() == kOfxImageEffectFileParamName) {
                computeFrameRangeForReader(knob.get());
            }
            
            //}
            break;
        }
    }
}

void
Node::restoreKnobsLinks(const NodeSerialization & serialization,
                        const std::vector<boost::shared_ptr<Natron::Node> > & allNodes)
{
    ////Only called by the main-thread
    assert( QThread::currentThread() == qApp->thread() );
    
    const NodeSerialization::KnobValues & knobsValues = serialization.getKnobsValues();
    ///try to find a serialized value for this knob
    for (NodeSerialization::KnobValues::const_iterator it = knobsValues.begin(); it != knobsValues.end(); ++it) {
        boost::shared_ptr<KnobI> knob = getKnobByName( (*it)->getName() );
        if (!knob) {
            appPTR->writeToOfxLog_mt_safe("Couldn't find a parameter named " + QString((*it)->getName().c_str()));
            continue;
        }
        (*it)->restoreKnobLinks(knob,allNodes);
        (*it)->restoreTracks(knob,allNodes);
    }
}

void
Node::setKnobsAge(U64 newAge)
{
    ////Only called by the main-thread
    assert( QThread::currentThread() == qApp->thread() );
    
    QWriteLocker l(&_imp->knobsAgeMutex);
    if (_imp->knobsAge != newAge) {
        _imp->knobsAge = newAge;
        emit knobsAgeChanged(_imp->knobsAge);
        l.unlock();
        computeHash();
        l.relock();
    }
}

void
Node::incrementKnobsAge()
{
    U32 newAge;
    {
        QWriteLocker l(&_imp->knobsAgeMutex);
        ++_imp->knobsAge;
        
        ///if the age of an effect somehow reaches the maximum age (will never happen)
        ///handle it by clearing the cache and resetting the age to 0.
        if ( _imp->knobsAge == std::numeric_limits<U64>::max() ) {
            appPTR->clearAllCaches();
            _imp->knobsAge = 0;
        }
        newAge = _imp->knobsAge;
    }
    emit knobsAgeChanged(newAge);
    
    computeHash();
}

U64
Node::getKnobsAge() const
{
    QReadLocker l(&_imp->knobsAgeMutex);
    
    return _imp->knobsAge;
}

bool
Node::isRenderingPreview() const
{
    QMutexLocker l(&_imp->computingPreviewMutex);
    
    return _imp->computingPreview;
}

void
Node::Implementation::abortPreview()
{
    bool computing;
    {
        QMutexLocker locker(&computingPreviewMutex);
        computing = computingPreview;
    }
    
    if (computing) {
        QMutexLocker l(&mustQuitPreviewMutex);
        mustQuitPreview = true;
        while (mustQuitPreview) {
            mustQuitPreviewCond.wait(&mustQuitPreviewMutex);
        }
    }
}

bool
Node::Implementation::checkForExitPreview()
{
    {
        QMutexLocker locker(&mustQuitPreviewMutex);
        if (mustQuitPreview) {
            mustQuitPreview = false;
            mustQuitPreviewCond.wakeOne();
            
            return true;
        }
        return false;
    }
}

void
Node::abortAnyProcessing()
{
    OutputEffectInstance* isOutput = dynamic_cast<OutputEffectInstance*>( getLiveInstance() );
    
    if (isOutput) {
        isOutput->getRenderEngine()->abortRendering(true);
    }
    _imp->abortPreview();
}

void
Node::quitAnyProcessing()
{
    {
        QMutexLocker k(&_imp->nodeIsDequeuingMutex);
        _imp->nodeIsDequeuing = false;
        _imp->nodeIsDequeuingCond.wakeAll();
    }
    
    {
        QMutexLocker k(&_imp->mustQuitProcessingMutex);
        _imp->mustQuitProcessing = true;
    }
    
    OutputEffectInstance* isOutput = dynamic_cast<OutputEffectInstance*>( getLiveInstance() );
    
    if (isOutput) {
        isOutput->getRenderEngine()->quitEngine();
    }
    _imp->abortPreview();
    
}

Node::~Node()
{
    delete _imp->liveInstance;
}

void
Node::removeReferences(bool ensureThreadsFinished)
{
    if (ensureThreadsFinished) {
        getApp()->getProject()->ensureAllProcessingThreadsFinished();
    }
    OutputEffectInstance* isOutput = dynamic_cast<OutputEffectInstance*>(_imp->liveInstance);
    
    if (isOutput) {
        isOutput->getRenderEngine()->quitEngine();
    }
    appPTR->removeAllImagesFromCacheWithMatchingKey( getHashValue() );
    delete _imp->liveInstance;
    _imp->liveInstance = 0;
}

const std::vector<std::string> &
Node::getInputLabels() const
{
    assert(_imp->inputsInitialized);
    ///MT-safe as it never changes.
    ////Only called by the main-thread
    assert( QThread::currentThread() == qApp->thread() );
    
    return _imp->inputLabels;
}

const std::list<Node* > &
Node::getOutputs() const
{
    ////Only called by the main-thread
    assert( QThread::currentThread() == qApp->thread() );
    
    return _imp->outputs;
}

void
Node::getOutputs_mt_safe(std::list<Node* >& outputs) const
{
    QMutexLocker l(&_imp->outputsMutex);
    outputs =  _imp->outputs;
}

void
Node::getInputNames(std::vector<std::string> & inputNames) const
{
    ///This is called by the serialization thread.
    ///We use the guiInputs because we want to serialize exactly how the tree was to the user
    
    if (_imp->multiInstanceParent) {
        _imp->multiInstanceParent->getInputNames(inputNames);
        return;
    }
    int maxInp = _imp->liveInstance->getMaxInputCount();
    
    QMutexLocker l(&_imp->inputsMutex);
    for (int i = 0; i < maxInp; ++i) {
        if (_imp->inputs[i]) {
            inputNames.push_back( _imp->inputs[i]->getName_mt_safe() );
        } else {
            inputNames.push_back("");
        }
    }
}

int
Node::getPreferredInputForConnection() 
{
    assert( QThread::currentThread() == qApp->thread() );
    if (getMaxInputCount() == 0) {
        return -1;
    }
    
    {
        ///Find an input named Source
        std::string inputNameToFind(kOfxImageEffectSimpleSourceClipName);
        int maxinputs = getMaxInputCount();
        for (int i = 0; i < maxinputs ; ++i) {
            if (getInputLabel(i) == inputNameToFind && !getInput(i)) {
                return i;
            }
        }
    }
    
    
    bool useInputA = appPTR->getCurrentSettings()->isMergeAutoConnectingToAInput();
    if (useInputA) {
        ///Find an input named A
        std::string inputNameToFind("A");
        int maxinputs = getMaxInputCount();
        for (int i = 0; i < maxinputs ; ++i) {
            if (getInputLabel(i) == inputNameToFind && !getInput(i)) {
                return i;
            }
        }
    }
    
   
    
    ///we return the first non-optional empty input
    int firstNonOptionalEmptyInput = -1;
    std::list<int> optionalEmptyInputs;
    std::list<int> optionalEmptyMasks;
    {
        QMutexLocker l(&_imp->inputsMutex);
        for (U32 i = 0; i < _imp->inputs.size(); ++i) {
            if (_imp->liveInstance->isInputRotoBrush(i)) {
                continue;
            }
            if (!_imp->inputs[i]) {
                if ( !_imp->liveInstance->isInputOptional(i) ) {
                    if (firstNonOptionalEmptyInput == -1) {
                        firstNonOptionalEmptyInput = i;
                        break;
                    }
                } else {
                    if (_imp->liveInstance->isInputMask(i)) {
                        optionalEmptyMasks.push_back(i);
                    } else {
                        optionalEmptyInputs.push_back(i);
                    }
                }
            }
        }
    }
    
   
    ///Default to the first non optional empty input
    if (firstNonOptionalEmptyInput != -1) {
        return firstNonOptionalEmptyInput;
    }  else {
        if ( !optionalEmptyInputs.empty() ) {
            
            //We return the last optional empty input
            std::list<int>::reverse_iterator first = optionalEmptyInputs.rbegin();
            while ( first != optionalEmptyInputs.rend() && _imp->liveInstance->isInputRotoBrush(*first) ) {
                ++first;
            }
            if ( first == optionalEmptyInputs.rend() ) {
                return -1;
            } else {
                return *first;
            }

        } else if (!optionalEmptyMasks.empty()) {
            return optionalEmptyMasks.front();
        } else {
            return -1;
        }
    }
}

void
Node::getOutputsConnectedToThisNode(std::map<Node*,int>* outputs)
{
    ////Only called by the main-thread
    assert( QThread::currentThread() == qApp->thread() );
    
    for (std::list<Node*>::iterator it = _imp->outputs.begin(); it != _imp->outputs.end(); ++it) {
        assert(*it);
        int indexOfThis = (*it)->inputIndex(this);
        assert(indexOfThis != -1);
        if (indexOfThis >= 0) {
            outputs->insert( std::make_pair(*it, indexOfThis) );
        }
    }
}

const std::string &
Node::getName() const
{
    ////Only called by the main-thread
    assert( QThread::currentThread() == qApp->thread() );
    QMutexLocker l(&_imp->nameMutex);
    
    return _imp->name;
}

std::string
Node::getName_mt_safe() const
{
    QMutexLocker l(&_imp->nameMutex);
    
    return _imp->name;
}

void
Node::setName(const QString & name)
{
    {
        QMutexLocker l(&_imp->nameMutex);
        _imp->name = name.toStdString();
    }
    emit nameChanged(name);
}

AppInstance*
Node::getApp() const
{
    return _imp->app;
}

bool
Node::isActivated() const
{
    QMutexLocker l(&_imp->activatedMutex);
    
    return _imp->activated;
}

std::string
Node::makeInfoForInput(int inputNumber) const
{
    const Natron::Node* inputNode = 0;
    std::string inputName ;
    if (inputNumber != -1) {
        inputNode = getInput(inputNumber).get();
        inputName = _imp->liveInstance->getInputLabel(inputNumber);
    } else {
        inputNode = this;
        inputName = "Output";
    }

    if (!inputNode) {
        return inputName + ": disconnected";
    }
    
    Natron::ImageComponentsEnum comps;
    Natron::ImageBitDepthEnum depth;
    Natron::ImagePremultiplicationEnum premult;
    
    double par = inputNode->getLiveInstance()->getPreferredAspectRatio();
    premult = inputNode->getLiveInstance()->getOutputPremultiplication();
    
    std::string premultStr;
    switch (premult) {
        case Natron::eImagePremultiplicationOpaque:
            premultStr = "opaque";
            break;
        case Natron::eImagePremultiplicationPremultiplied:
            premultStr= "premultiplied";
            break;
        case Natron::eImagePremultiplicationUnPremultiplied:
            premultStr= "unpremultiplied";
            break;
    }
    
    _imp->liveInstance->getPreferredDepthAndComponents(inputNumber, &comps, &depth);
    
    RenderScale scale;
    scale.x = scale.y = 1.;
    RectD rod;
    bool isProjectFormat;
    StatusEnum stat = inputNode->getLiveInstance()->getRegionOfDefinition_public(getHashValue(),
                                                                                 inputNode->getLiveInstance()->getCurrentTime(),
                                                                                 scale, 0, &rod, &isProjectFormat);
    
    
    std::stringstream ss;
    ss << "<b><font color=\"orange\">"<< inputName << ":\n" << "</font></b>"
    << "<b>Image Format:</b> " << Natron::Image::getFormatString(comps, depth)
    << "\n<b>Alpha premultiplication:</b> " << premultStr
    << "\n<b>Pixel aspect ratio:</b> " << par;
    if (stat != Natron::eStatusFailed) {
        ss << "\n<b>Region of Definition:</b> ";
        ss << "left = " << rod.x1 << " bottom = " << rod.y1 << " right = " << rod.x2 << " top = " << rod.y2 << '\n';
    }
    return ss.str();
}

void
Node::initializeKnobs(const NodeSerialization & serialization,int renderScaleSupportPref)
{
    ////Only called by the main-thread
    _imp->liveInstance->beginChanges();
    
    assert( QThread::currentThread() == qApp->thread() );
    assert(!_imp->knobsInitialized);
    _imp->liveInstance->initializeKnobsPublic();
    
    ///If the effect has a mask, add additionnal mask controls
    int inputsCount = getMaxInputCount();
    for (int i = 0; i < inputsCount; ++i) {
        if ( _imp->liveInstance->isInputMask(i) && !_imp->liveInstance->isInputRotoBrush(i) ) {
            std::string maskName = _imp->liveInstance->getInputLabel(i);
            boost::shared_ptr<Bool_Knob> enableMaskKnob = Natron::createKnob<Bool_Knob>(_imp->liveInstance, maskName,1,false);
            _imp->enableMaskKnob.insert( std::make_pair(i,enableMaskKnob) );
            enableMaskKnob->setDefaultValue(false, 0);
            enableMaskKnob->turnOffNewLine();
            std::string enableMaskName(std::string(kEnableMaskKnobName) + std::string("_") + maskName);
            enableMaskKnob->setName(enableMaskName);
            enableMaskKnob->setAnimationEnabled(false);
            enableMaskKnob->setHintToolTip("Enable the mask to come from the channel named by the choice parameter on the right. "
                                           "Turning this off will act as though the mask was disconnected.");
            
            boost::shared_ptr<Choice_Knob> maskChannelKnob = Natron::createKnob<Choice_Knob>(_imp->liveInstance, "",1,false);
            _imp->maskChannelKnob.insert( std::make_pair(i,maskChannelKnob) );
            std::vector<std::string> choices;
            choices.push_back("None");
            choices.push_back("Red");
            choices.push_back("Green");
            choices.push_back("Blue");
            choices.push_back("Alpha");
            maskChannelKnob->populateChoices(choices);
            maskChannelKnob->setDefaultValue(4, 0);
            maskChannelKnob->setAnimationEnabled(false);
            maskChannelKnob->turnOffNewLine();
            maskChannelKnob->setHintToolTip("Use this channel from the original input to mix the output with the original input. "
                                            "Setting this to None is the same as disabling the mask.");
            std::string channelMaskName(kMaskChannelKnobName + std::string("_") + maskName);
            maskChannelKnob->setName(channelMaskName);
            
            
            ///and load it
            loadKnob(enableMaskKnob, serialization);
            loadKnob(maskChannelKnob, serialization);
        }
    }
    
    _imp->nodeSettingsPage = Natron::createKnob<Page_Knob>(_imp->liveInstance, NATRON_EXTRA_PARAMETER_PAGE_NAME,1,false);
    
    _imp->nodeLabelKnob = Natron::createKnob<String_Knob>(_imp->liveInstance, "Label",1,false);
    assert(_imp->nodeLabelKnob);
    _imp->nodeLabelKnob->setName(kUserLabelKnobName);
    _imp->nodeLabelKnob->setAnimationEnabled(false);
    _imp->nodeLabelKnob->setEvaluateOnChange(false);
    _imp->nodeLabelKnob->setAsMultiLine();
    _imp->nodeLabelKnob->setUsesRichText(true);
    _imp->nodeLabelKnob->setHintToolTip("This label gets appended to the node name on the node graph.");
    _imp->nodeSettingsPage->addKnob(_imp->nodeLabelKnob);
    loadKnob(_imp->nodeLabelKnob, serialization);
    
    _imp->forceCaching = Natron::createKnob<Bool_Knob>(_imp->liveInstance, "Force caching", 1, false);
    _imp->forceCaching->setName("forceCaching");
    _imp->forceCaching->setDefaultValue(false);
    _imp->forceCaching->setAnimationEnabled(false);
    _imp->forceCaching->turnOffNewLine();
    _imp->forceCaching->setIsPersistant(true);
    _imp->forceCaching->setEvaluateOnChange(false);
    _imp->forceCaching->setHintToolTip("When checked, the output of this node will always be kept in the RAM cache for fast access of already computed "
                                       "images.");
    _imp->nodeSettingsPage->addKnob(_imp->forceCaching);
    loadKnob(_imp->forceCaching,serialization);
    
    _imp->previewEnabledKnob = Natron::createKnob<Bool_Knob>(_imp->liveInstance, "Preview enabled",1,false);
    assert(_imp->previewEnabledKnob);
    _imp->previewEnabledKnob->setDefaultValue( makePreviewByDefault() );
    _imp->previewEnabledKnob->setName(kEnablePreviewKnobName);
    _imp->previewEnabledKnob->setAnimationEnabled(false);
    _imp->previewEnabledKnob->turnOffNewLine();
    _imp->previewEnabledKnob->setIsPersistant(false);
    _imp->previewEnabledKnob->setEvaluateOnChange(false);
    _imp->previewEnabledKnob->setHintToolTip("Whether to show a preview on the node box in the node-graph.");
    _imp->nodeSettingsPage->addKnob(_imp->previewEnabledKnob);
    
    _imp->disableNodeKnob = Natron::createKnob<Bool_Knob>(_imp->liveInstance, "Disable",1,false);
    assert(_imp->disableNodeKnob);
    _imp->disableNodeKnob->setAnimationEnabled(false);
    _imp->disableNodeKnob->setDefaultValue(false);
    _imp->disableNodeKnob->setName(kDisableNodeKnobName);
    _imp->disableNodeKnob->turnOffNewLine();
    _imp->disableNodeKnob->setHintToolTip("When disabled, this node acts as a pass through.");
    _imp->nodeSettingsPage->addKnob(_imp->disableNodeKnob);
    loadKnob(_imp->disableNodeKnob, serialization);
    
    _imp->useFullScaleImagesWhenRenderScaleUnsupported = Natron::createKnob<Bool_Knob>(_imp->liveInstance, "Render high def. upstream",1,false);
    _imp->useFullScaleImagesWhenRenderScaleUnsupported->setAnimationEnabled(false);
    _imp->useFullScaleImagesWhenRenderScaleUnsupported->setDefaultValue(false);
    _imp->useFullScaleImagesWhenRenderScaleUnsupported->setName("highDefUpstream");
    _imp->useFullScaleImagesWhenRenderScaleUnsupported->setHintToolTip("This node doesn't support rendering images at a scale lower than 1, it "
                                                                       "can only render high definition images. When checked this parameter controls "
                                                                       "whether the rest of the graph upstream should be rendered with a high quality too or at "
                                                                       "the most optimal resolution for the current viewer's viewport. Typically checking this "
                                                                       "means that an image will be slow to be rendered, but once rendered it will stick in the cache "
                                                                       "whichever zoom level you're using on the Viewer, whereas when unchecked it will be much "
                                                                       "faster to render but will have to be recomputed when zooming in/out in the Viewer.");
    if (renderScaleSupportPref == 1) {
        _imp->useFullScaleImagesWhenRenderScaleUnsupported->setSecret(true);
    }
    _imp->nodeSettingsPage->addKnob(_imp->useFullScaleImagesWhenRenderScaleUnsupported);
    loadKnob(_imp->useFullScaleImagesWhenRenderScaleUnsupported, serialization);
    
    _imp->infoPage = Natron::createKnob<Page_Knob>(_imp->liveInstance, "Info",1,false);
    _imp->infoPage->setName("info");
    
    _imp->infoDisclaimer = Natron::createKnob<String_Knob>(_imp->liveInstance, "Input and output informations",1,false);
    _imp->infoDisclaimer->setName("infoDisclaimer");
    _imp->infoDisclaimer->setAnimationEnabled(false);
    _imp->infoDisclaimer->setIsPersistant(false);
    _imp->infoDisclaimer->setAsLabel();
    _imp->infoDisclaimer->hideDescription();
    _imp->infoDisclaimer->setEvaluateOnChange(false);
    _imp->infoDisclaimer->setDefaultValue(tr("Input and output informations, press Refresh to update them with current values").toStdString());
    _imp->infoPage->addKnob(_imp->infoDisclaimer);
    
    for (int i = 0; i < inputsCount; ++i) {
        std::string inputLabel = getInputLabel(i);
        boost::shared_ptr<String_Knob> inputInfo = Natron::createKnob<String_Knob>(_imp->liveInstance, std::string(inputLabel + " Info"), 1, false);
        inputInfo->setName(inputLabel + "Info");
        inputInfo->setAnimationEnabled(false);
        inputInfo->setIsPersistant(false);
        inputInfo->setEvaluateOnChange(false);
        inputInfo->hideDescription();
        inputInfo->setAsLabel();
        _imp->inputFormats.push_back(inputInfo);
        _imp->infoPage->addKnob(inputInfo);
    }
    
    std::string outputLabel("Output");
    _imp->outputFormat = Natron::createKnob<String_Knob>(_imp->liveInstance, std::string(outputLabel + " Info"), 1, false);
    _imp->outputFormat->setName(outputLabel + "Info");
    _imp->outputFormat->setAnimationEnabled(false);
    _imp->outputFormat->setIsPersistant(false);
    _imp->outputFormat->setEvaluateOnChange(false);
    _imp->outputFormat->hideDescription();
    _imp->outputFormat->setAsLabel();
    _imp->infoPage->addKnob(_imp->outputFormat);
    
    _imp->refreshInfoButton = Natron::createKnob<Button_Knob>(_imp->liveInstance, "Refresh Info");
    _imp->refreshInfoButton->setName("refreshButton");
    _imp->refreshInfoButton->setEvaluateOnChange(false);
    _imp->infoPage->addKnob(_imp->refreshInfoButton);
    
    
    _imp->knobsInitialized = true;
    _imp->liveInstance->endChanges();
    emit knobsInitialized();
} // initializeKnobs

bool
Node::isForceCachingEnabled() const
{
    return _imp->forceCaching->getValue();
}

void
Node::onSetSupportRenderScaleMaybeSet(int support)
{
    if ((EffectInstance::SupportsEnum)support == EffectInstance::eSupportsYes) {
        if (_imp->useFullScaleImagesWhenRenderScaleUnsupported) {
            _imp->useFullScaleImagesWhenRenderScaleUnsupported->setSecret(true);
        }
    }
}

bool
Node::useScaleOneImagesWhenRenderScaleSupportIsDisabled() const
{
    return _imp->useFullScaleImagesWhenRenderScaleUnsupported->getValue();
}

void
Node::beginEditKnobs()
{
    _imp->liveInstance->beginEditKnobs();
}

void
Node::createKnobDynamically()
{
    emit knobsInitialized();
}

void
Node::setLiveInstance(Natron::EffectInstance* liveInstance)
{
    ////Only called by the main-thread
    assert( QThread::currentThread() == qApp->thread() );
    _imp->liveInstance = liveInstance;
    _imp->liveInstance->initializeData();
}

Natron::EffectInstance*
Node::getLiveInstance() const
{
    ///Thread safe as it never changes
    return _imp->liveInstance;
}

bool
Node::hasEffect() const
{
    return _imp->liveInstance != NULL;
}

void
Node::hasViewersConnected(std::list<ViewerInstance* >* viewers) const
{
    ViewerInstance* thisViewer = dynamic_cast<ViewerInstance*>(_imp->liveInstance);
    
    if (thisViewer) {
        std::list<ViewerInstance* >::const_iterator alreadyExists = std::find(viewers->begin(), viewers->end(), thisViewer);
        if ( alreadyExists == viewers->end() ) {
            viewers->push_back(thisViewer);
        }
    } else {
        if ( QThread::currentThread() == qApp->thread() ) {
            for (std::list<Node*>::iterator it = _imp->outputs.begin(); it != _imp->outputs.end(); ++it) {
                assert(*it);
                (*it)->hasViewersConnected(viewers);
            }
        } else {
            QMutexLocker l(&_imp->outputsMutex);
            for (std::list<Node*>::iterator it = _imp->outputs.begin(); it != _imp->outputs.end(); ++it) {
                assert(*it);
                (*it)->hasViewersConnected(viewers);
            }
        }
    }
}

void
Node::hasWritersConnected(std::list<Natron::OutputEffectInstance* >* writers) const
{
    Natron::OutputEffectInstance* thisWriter = dynamic_cast<Natron::OutputEffectInstance*>(_imp->liveInstance);
    
    if (thisWriter) {
        std::list<Natron::OutputEffectInstance* >::const_iterator alreadyExists = std::find(writers->begin(), writers->end(), thisWriter);
        if ( alreadyExists == writers->end() ) {
            writers->push_back(thisWriter);
        }
    } else {
        if ( QThread::currentThread() == qApp->thread() ) {
            for (std::list<Node*>::iterator it = _imp->outputs.begin(); it != _imp->outputs.end(); ++it) {
                assert(*it);
                (*it)->hasWritersConnected(writers);
            }
        } else {
            QMutexLocker l(&_imp->outputsMutex);
            for (std::list<Node*>::iterator it = _imp->outputs.begin(); it != _imp->outputs.end(); ++it) {
                assert(*it);
                (*it)->hasWritersConnected(writers);
            }
        }
    }
}

int
Node::getMajorVersion() const
{
    ///Thread safe as it never changes
    return _imp->liveInstance->getMajorVersion();
}

int
Node::getMinorVersion() const
{
    ///Thread safe as it never changes
    return _imp->liveInstance->getMinorVersion();
}

void
Node::initializeInputs()
{
    ////Only called by the main-thread
    assert( QThread::currentThread() == qApp->thread() );
    
    int oldCount = (int)_imp->inputs.size();
    int inputCount = getMaxInputCount();
    
    {
        QMutexLocker l(&_imp->inputsMutex);
        _imp->inputs.resize(inputCount);
        _imp->inputLabels.resize(inputCount);
        ///if we added inputs, just set to NULL the new inputs, and add their label to the labels map
        if (inputCount > oldCount) {
            for (int i = oldCount; i < inputCount; ++i) {
                _imp->inputLabels[i] = _imp->liveInstance->getInputLabel(i);
                _imp->inputs[i].reset();
            }
        }
        
        ///Set the components the plug-in accepts
        _imp->inputsComponents.resize(inputCount);
        for (int i = 0; i < inputCount; ++i) {
            _imp->inputsComponents[i].clear();
            _imp->liveInstance->addAcceptedComponents(i, &_imp->inputsComponents[i]);
        }
        _imp->outputComponents.clear();
        _imp->liveInstance->addAcceptedComponents(-1, &_imp->outputComponents);
    }
    _imp->inputsInitialized = true;
    emit inputsInitialized();
}

boost::shared_ptr<Node>
Node::getInput(int index) const
{
    if (_imp->multiInstanceParent) {
        return _imp->multiInstanceParent->getInput(index);
    }
    if (!_imp->inputsInitialized) {
        qDebug() << "Node::getInput(): inputs not initialized";
    }
    QMutexLocker l(&_imp->inputsMutex);
    if ( ( index >= (int)_imp->inputs.size() ) || (index < 0) ) {
        return boost::shared_ptr<Node>();
    }
    
    return _imp->inputs[index];
}

const std::vector<boost::shared_ptr<Natron::Node> > &
Node::getInputs_mt_safe() const
{
    ////Only called by the main-thread
    assert( QThread::currentThread() == qApp->thread() );
    assert(_imp->inputsInitialized);
    
    if (_imp->multiInstanceParent) {
        return _imp->multiInstanceParent->getInputs_mt_safe();
    }
    
    return _imp->inputs;
}

std::vector<boost::shared_ptr<Natron::Node> >
Node::getInputs_copy() const
{
    assert(_imp->inputsInitialized);
    
    if (_imp->multiInstanceParent) {
        return _imp->multiInstanceParent->getInputs_mt_safe();
    }
    
    QMutexLocker l(&_imp->inputsMutex);
    return _imp->inputs;
}

std::string
Node::getInputLabel(int inputNb) const
{
    assert(_imp->inputsInitialized);
    
    QMutexLocker l(&_imp->inputsMutex);
    if ( (inputNb < 0) || ( inputNb >= (int)_imp->inputLabels.size() ) ) {
        throw std::invalid_argument("Index out of range");
    }
    
    return _imp->inputLabels[inputNb];
}

bool
Node::isInputConnected(int inputNb) const
{
    assert(_imp->inputsInitialized);
    
    return getInput(inputNb) != NULL;
}

bool
Node::hasInputConnected() const
{
    assert(_imp->inputsInitialized);
    
    if (_imp->multiInstanceParent) {
        return _imp->multiInstanceParent->hasInputConnected();
    }
    QMutexLocker l(&_imp->inputsMutex);
    for (U32 i = 0; i < _imp->inputs.size(); ++i) {
        if (_imp->inputs[i]) {
            return true;
        }
    }
    
    
    return false;
}

bool
Node::hasMandatoryInputDisconnected() const
{
    QMutexLocker l(&_imp->inputsMutex);
    
    for (U32 i = 0; i < _imp->inputs.size(); ++i) {
        if (!_imp->inputs[i] && !_imp->liveInstance->isInputOptional(i)) {
            return true;
        }
    }
    return false;
}

bool
Node::hasOutputConnected() const
{
    ////Only called by the main-thread
    if (_imp->multiInstanceParent) {
        return _imp->multiInstanceParent->hasInputConnected();
    }
    if ( QThread::currentThread() == qApp->thread() ) {
        return _imp->outputs.size() > 0;
    } else {
        QMutexLocker l(&_imp->outputsMutex);
        
        return _imp->outputs.size() > 0;
    }
}

bool
Node::checkIfConnectingInputIsOk(Natron::Node* input) const
{
    ////Only called by the main-thread
    assert( QThread::currentThread() == qApp->thread() );
    if (input == this) {
        return false;
    }
    bool found;
    input->isNodeUpstream(this, &found);
    
    return !found;
}

void
Node::isNodeUpstream(const Natron::Node* input,
                     bool* ok) const
{
    ////Only called by the main-thread
    assert( QThread::currentThread() == qApp->thread() );
    
    if (!input) {
        *ok = false;
        
        return;
    }
    
    ///No need to lock guiInputs is only written to by the main-thread
    
    for (U32 i = 0; i  < _imp->inputs.size(); ++i) {
        if (_imp->inputs[i].get() == input) {
            *ok = true;
            
            return;
        }
    }
    *ok = false;
    for (U32 i = 0; i  < _imp->inputs.size(); ++i) {
        if (_imp->inputs[i]) {
            _imp->inputs[i]->isNodeUpstream(input, ok);
            if (*ok) {
                return;
            }
        }
    }
}

Node::CanConnectInputReturnValue
Node::canConnectInput(const boost::shared_ptr<Node>& input,int inputNumber) const
{
   
    
    
    ///No-one is allowed to connect to the other node
    if (!input->canOthersConnectToThisNode()) {
        return eCanConnectInput_givenNodeNotConnectable;
    }
    
    ///Applying this connection would create cycles in the graph
    if (!checkIfConnectingInputIsOk(input.get())) {
        return eCanConnectInput_graphCycles;
    }
    
    if (_imp->liveInstance->isInputRotoBrush(inputNumber)) {
        qDebug() << "Debug: Attempt to connect " << input->getName_mt_safe().c_str() << " to Roto brush";
        return eCanConnectInput_indexOutOfRange;
    }
    
    {
        ///Check for invalid index
        QMutexLocker l(&_imp->inputsMutex);
        if ( (inputNumber < 0) || ( inputNumber >= (int)_imp->inputs.size() )) {
            return eCanConnectInput_indexOutOfRange;
        }
        if (_imp->inputs[inputNumber]) {
            return eCanConnectInput_inputAlreadyConnected;
        }
        
        ///Check for invalid pixel aspect ratio if the node doesn't support multiple clip PARs
        if (!_imp->liveInstance->supportsMultipleClipsPAR()) {
            
            double inputPAR = input->getLiveInstance()->getPreferredAspectRatio();
            
            double inputFPS = input->getLiveInstance()->getPreferredFrameRate();
            
            for (InputsV::const_iterator it = _imp->inputs.begin(); it != _imp->inputs.end(); ++it) {
                if (*it) {
                    if ((*it)->getLiveInstance()->getPreferredAspectRatio() != inputPAR) {
                        return eCanConnectInput_differentPars;
                    }
                    
                    if (std::abs((*it)->getLiveInstance()->getPreferredFrameRate() - inputFPS) > 0.01) {
                        return eCanConnectInput_differentFPS;
                    }
                    
                }
            }
        }
    }
    
    return eCanConnectInput_ok;
}

bool
Node::connectInput(const boost::shared_ptr<Node> & input,
                   int inputNumber)
{
    ////Only called by the main-thread
    assert( QThread::currentThread() == qApp->thread() );
    assert(_imp->inputsInitialized);
    assert(input);
    
    ///Check for cycles: they are forbidden in the graph
    if ( !checkIfConnectingInputIsOk( input.get() ) ) {
        return false;
    }
    if (_imp->liveInstance->isInputRotoBrush(inputNumber)) {
        qDebug() << "Debug: Attempt to connect " << input->getName_mt_safe().c_str() << " to Roto brush";
        return false;
    }
    {
        ///Check for invalid index
        QMutexLocker l(&_imp->inputsMutex);
        if ( (inputNumber < 0) || ( inputNumber >= (int)_imp->inputs.size() ) || (_imp->inputs[inputNumber]) ) {
            return false;
        }
        
        ///If the node is currently rendering, queue the action instead of executing it
        {
            QMutexLocker k(&_imp->nodeIsRenderingMutex);
            if (_imp->nodeIsRendering > 0 && !appPTR->isBackground()) {
                ConnectInputAction action(input,eInputActionConnect,inputNumber);
                QMutexLocker cql(&_imp->connectionQueueMutex);
                _imp->connectionQueue.push_back(action);
                return true;
            }
        }
        
        ///Set the input
        _imp->inputs[inputNumber] = input;
        input->connectOutput(this);
    }
    
    ///Get notified when the input name has changed
    QObject::connect( input.get(), SIGNAL( nameChanged(QString) ), this, SLOT( onInputNameChanged(QString) ) );
    
    ///Notify the GUI
    emit inputChanged(inputNumber);
    
    ///Call the instance changed action with a reason clip changed
    onInputChanged(inputNumber);
    
    ///Recompute the hash
    computeHash();
    
    return true;
}

bool
Node::replaceInput(const boost::shared_ptr<Node>& input,int inputNumber)
{
    ////Only called by the main-thread
    assert( QThread::currentThread() == qApp->thread() );
    assert(_imp->inputsInitialized);
    assert(input);
    
    ///Check for cycles: they are forbidden in the graph
    if ( !checkIfConnectingInputIsOk( input.get() ) ) {
        return false;
    }
    if (_imp->liveInstance->isInputRotoBrush(inputNumber)) {
        qDebug() << "Debug: Attempt to connect " << input->getName_mt_safe().c_str() << " to Roto brush";
        return false;
    }
    {
        ///Check for invalid index
        QMutexLocker l(&_imp->inputsMutex);
        if ( (inputNumber < 0) || ( inputNumber > (int)_imp->inputs.size() ) ) {
            return false;
        }
        
        ///If the node is currently rendering, queue the action instead of executing it
        {
            QMutexLocker k(&_imp->nodeIsRenderingMutex);
            if (_imp->nodeIsRendering > 0 && !appPTR->isBackground()) {
                ConnectInputAction action(input,eInputActionReplace,inputNumber);
                QMutexLocker cql(&_imp->connectionQueueMutex);
                _imp->connectionQueue.push_back(action);
                return true;
            }
        }
        
        ///Set the input
        if (_imp->inputs[inputNumber]) {
            QObject::connect( _imp->inputs[inputNumber].get(), SIGNAL( nameChanged(QString) ), this, SLOT( onInputNameChanged(QString) ) );
            _imp->inputs[inputNumber]->disconnectOutput(this);
        }
        _imp->inputs[inputNumber] = input;
        input->connectOutput(this);
    }
    
    ///Get notified when the input name has changed
    QObject::connect( input.get(), SIGNAL( nameChanged(QString) ), this, SLOT( onInputNameChanged(QString) ) );
    
    ///Notify the GUI
    emit inputChanged(inputNumber);
    
    ///Call the instance changed action with a reason clip changed
    onInputChanged(inputNumber);
    
    ///Recompute the hash
    computeHash();
    return true;
}

void
Node::switchInput0And1()
{
    ////Only called by the main-thread
    assert( QThread::currentThread() == qApp->thread() );
    assert(_imp->inputsInitialized);
    int maxInputs = getMaxInputCount();
    if (maxInputs < 2) {
        return;
    }
    ///get the first input number to switch
    int inputAIndex = -1;
    for (int i = 0; i < maxInputs; ++i) {
        if ( !_imp->liveInstance->isInputMask(i) ) {
            inputAIndex = i;
            break;
        }
    }
    
    ///There's only a mask ??
    if (inputAIndex == -1) {
        return;
    }
    
    ///get the second input number to switch
    int inputBIndex = -1;
    int firstMaskInput = -1;
    for (int j = 0; j < maxInputs; ++j) {
        if (j == inputAIndex) {
            continue;
        }
        if ( !_imp->liveInstance->isInputMask(j) ) {
            inputBIndex = j;
            break;
        } else {
            firstMaskInput = j;
        }
    }
    if (inputBIndex == -1) {
        ///if there's a mask use it as input B for the switch
        if (firstMaskInput != -1) {
            inputBIndex = firstMaskInput;
        } 
    }
    
    ///If the node is currently rendering, queue the action instead of executing it
    {
        QMutexLocker k(&_imp->nodeIsRenderingMutex);
        if (_imp->nodeIsRendering > 0 && !appPTR->isBackground()) {
            QMutexLocker cql(&_imp->connectionQueueMutex);
            ///Replace input A
            {
                ConnectInputAction action(_imp->inputs[inputBIndex],eInputActionReplace,inputAIndex);
                _imp->connectionQueue.push_back(action);
            }
            
            ///Replace input B
            {
                ConnectInputAction action(_imp->inputs[inputAIndex],eInputActionReplace,inputBIndex);
                _imp->connectionQueue.push_back(action);
            }
            
            return;
        }
    }
    {
        QMutexLocker l(&_imp->inputsMutex);
        assert( inputAIndex < (int)_imp->inputs.size() && inputBIndex < (int)_imp->inputs.size() );
        boost::shared_ptr<Natron::Node> input0 = _imp->inputs[inputAIndex];
        _imp->inputs[inputAIndex] = _imp->inputs[inputBIndex];
        _imp->inputs[inputBIndex] = input0;
    }
    emit inputChanged(inputAIndex);
    emit inputChanged(inputBIndex);
    onInputChanged(inputAIndex);
    onInputChanged(inputBIndex);
    computeHash();
} // switchInput0And1

void
Node::onInputNameChanged(const QString & name)
{
    assert( QThread::currentThread() == qApp->thread() );
    assert(_imp->inputsInitialized);
    Natron::Node* inp = dynamic_cast<Natron::Node*>( sender() );
    assert(inp);
    if (!inp) {
        // coverity[dead_error_line]
        return;
    }
    int inputNb = -1;
    ///No need to lock, guiInputs is only written to by the mainthread
    
    for (U32 i = 0; i < _imp->inputs.size(); ++i) {
        if (_imp->inputs[i].get() == inp) {
            inputNb = i;
            break;
        }
    }
    
    if (inputNb != -1) {
        emit inputNameChanged(inputNb, name);
    }
}

void
Node::connectOutput(Node* output)
{
    ////Only called by the main-thread
    assert( QThread::currentThread() == qApp->thread() );
    assert(output);
    
    {
        QMutexLocker l(&_imp->outputsMutex);
        _imp->outputs.push_back(output);
    }
    emit outputsChanged();
}

int
Node::disconnectInput(int inputNumber)
{
    ////Only called by the main-thread
    assert( QThread::currentThread() == qApp->thread() );
    assert(_imp->inputsInitialized);
    
    {
        QMutexLocker l(&_imp->inputsMutex);
        if ( (inputNumber < 0) || ( inputNumber > (int)_imp->inputs.size() ) || (_imp->inputs[inputNumber] == NULL) ) {
            return -1;
        }
        
        ///If the node is currently rendering, queue the action instead of executing it
        {
            QMutexLocker k(&_imp->nodeIsRenderingMutex);
            if (_imp->nodeIsRendering > 0 && !appPTR->isBackground()) {
                ConnectInputAction action(_imp->inputs[inputNumber],eInputActionDisconnect,inputNumber);
                QMutexLocker cql(&_imp->connectionQueueMutex);
                _imp->connectionQueue.push_back(action);
                return inputNumber;
            }
        }
        
        QObject::disconnect( _imp->inputs[inputNumber].get(), SIGNAL( nameChanged(QString) ), this, SLOT( onInputNameChanged(QString) ) );
        _imp->inputs[inputNumber]->disconnectOutput(this);
        _imp->inputs[inputNumber].reset();
        
    }
    emit inputChanged(inputNumber);
    onInputChanged(inputNumber);
    computeHash();
    
    return inputNumber;
}

int
Node::disconnectInput(Node* input)
{
    ////Only called by the main-thread
    assert( QThread::currentThread() == qApp->thread() );
    assert(_imp->inputsInitialized);
    {
        QMutexLocker l(&_imp->inputsMutex);
        for (U32 i = 0; i < _imp->inputs.size(); ++i) {
            if (_imp->inputs[i].get() == input) {
                
                ///If the node is currently rendering, queue the action instead of executing it
                {
                    QMutexLocker k(&_imp->nodeIsRenderingMutex);
                    if (_imp->nodeIsRendering > 0 && !appPTR->isBackground()) {
                        ConnectInputAction action(_imp->inputs[i],eInputActionDisconnect,i);
                        QMutexLocker cql(&_imp->connectionQueueMutex);
                        _imp->connectionQueue.push_back(action);
                        return i;
                    }
                }
                
                _imp->inputs[i].reset();
                l.unlock();
                input->disconnectOutput(this);
                emit inputChanged(i);
                onInputChanged(i);
                computeHash();
                l.relock();
                
                return i;
            }
        }
    }
    
    return -1;
}

int
Node::disconnectOutput(Node* output)
{
    assert(output);
    ////Only called by the main-thread
    assert( QThread::currentThread() == qApp->thread() );
    int ret = -1;
    {
        QMutexLocker l(&_imp->outputsMutex);
        std::list<Node*>::iterator it = std::find(_imp->outputs.begin(),_imp->outputs.end(),output);
        
        if ( it != _imp->outputs.end() ) {
            ret = std::distance(_imp->outputs.begin(), it);
            _imp->outputs.erase(it);
        }
    }
    emit outputsChanged();
    
    return ret;
}


int
Node::inputIndex(Node* n) const
{
    if (!n) {
        return -1;
    }
    
    ///Only called by the main-thread
    assert( QThread::currentThread() == qApp->thread() );
    assert(_imp->inputsInitialized);
    
    if (_imp->multiInstanceParent) {
        return _imp->multiInstanceParent->inputIndex(n);
    }
    
    ///No need to lock this is only called by the main-thread
    for (U32 i = 0; i < _imp->inputs.size(); ++i) {
        if (_imp->inputs[i].get() == n) {
            return i;
        }
    }
    
    
    return -1;
}

void
Node::clearLastRenderedImage()
{
    _imp->liveInstance->clearLastRenderedImage();
}

/*After this call this node still knows the link to the old inputs/outputs
 but no other node knows this node.*/
void
Node::deactivate(const std::list< Node* > & outputsToDisconnect,
                 bool disconnectAll,
                 bool reconnect,
                 bool hideGui,
                 bool triggerRender)
{
    ///Only called by the main-thread
    assert( QThread::currentThread() == qApp->thread() );
    
    if (!_imp->liveInstance) {
        return;
    }
    
    //first tell the gui to clear any persistent message linked to this node
    clearPersistentMessage(false);
    
    ///For all knobs that have listeners, kill expressions
    const std::vector<boost::shared_ptr<KnobI> > & knobs = getKnobs();
    for (U32 i = 0; i < knobs.size(); ++i) {
        std::list<KnobI*> listeners;
        knobs[i]->getListeners(listeners);
        for (std::list<KnobI*>::iterator it = listeners.begin(); it != listeners.end(); ++it) {
            for (int dim = 0; dim < (*it)->getDimension(); ++dim) {
                std::pair<int, boost::shared_ptr<KnobI> > master = (*it)->getMaster(dim);
                if (master.second == knobs[i]) {
                    (*it)->unSlave(dim, true);
                }
            }
        }
    }
    
    ///if the node has 1 non-optional input, attempt to connect the outputs to the input of the current node
    ///this node is the node the outputs should attempt to connect to
    boost::shared_ptr<Node> inputToConnectTo;
    boost::shared_ptr<Node> firstOptionalInput;
    int firstNonOptionalInput = -1;
    if (reconnect) {
        bool hasOnlyOneInputConnected = false;
        
        ///No need to lock guiInputs is only written to by the mainthread
        for (U32 i = 0; i < _imp->inputs.size(); ++i) {
            if (_imp->inputs[i]) {
                if ( !_imp->liveInstance->isInputOptional(i) ) {
                    if (firstNonOptionalInput == -1) {
                        firstNonOptionalInput = i;
                        hasOnlyOneInputConnected = true;
                    } else {
                        hasOnlyOneInputConnected = false;
                    }
                } else if (!firstOptionalInput) {
                    firstOptionalInput = _imp->inputs[i];
                    if (hasOnlyOneInputConnected) {
                        hasOnlyOneInputConnected = false;
                    } else {
                        hasOnlyOneInputConnected = true;
                    }
                }
            }
        }
        
        if (hasOnlyOneInputConnected) {
            if (firstNonOptionalInput != -1) {
                inputToConnectTo = getInput(firstNonOptionalInput);
            } else if (firstOptionalInput) {
                inputToConnectTo = firstOptionalInput;
            }
        }
    }
    /*Removing this node from the output of all inputs*/
    _imp->deactivatedState.clear();
    
    
    std::vector<boost::shared_ptr<Node> > inputsQueueCopy;
    
    ///For multi-instances, if we deactivate the main instance without hiding the GUI (the default state of the tracker node)
    ///then don't remove it from outputs of the inputs
    if (hideGui || !_imp->isMultiInstance) {
        for (U32 i = 0; i < _imp->inputs.size(); ++i) {
            if (_imp->inputs[i]) {
                _imp->inputs[i]->disconnectOutput(this);
            }
        }
    }
    
    
    ///For each output node we remember that the output node  had its input number inputNb connected
    ///to this node
    std::list<Node*> outputsQueueCopy;
    {
        QMutexLocker l(&_imp->outputsMutex);
        outputsQueueCopy = _imp->outputs;
    }
    
    
    for (std::list<Node*>::iterator it = outputsQueueCopy.begin(); it != outputsQueueCopy.end(); ++it) {
        assert(*it);
        bool dc = false;
        if (disconnectAll) {
            dc = true;
        } else {
            
            for (std::list<Node* >::const_iterator found = outputsToDisconnect.begin(); found != outputsToDisconnect.end(); ++found) {
                if (*found == *it) {
                    dc = true;
                    break;
                }
            }
        }
        if (dc) {
            int inputNb = (*it)->disconnectInput(this);
            _imp->deactivatedState.insert( make_pair(*it, inputNb) );
            
            ///reconnect if inputToConnectTo is not null
            if (inputToConnectTo) {
                getApp()->getProject()->connectNodes(inputNb, inputToConnectTo, *it);
            }
        }
    }
    
    
    ///kill any thread it could have started
    ///Commented-out: If we were to undo the deactivate we don't want all threads to be
    ///exited, just exit them when the effect is really deleted instead
    //quitAnyProcessing();
    abortAnyProcessing();
    
    ///Free all memory used by the plug-in.
    
    ///COMMENTED-OUT: Don't do this, the node may still be rendering here.
    ///_imp->liveInstance->clearPluginMemoryChunks();
    clearLastRenderedImage();
    
    if (hideGui) {
        emit deactivated(triggerRender);
    }
    {
        QMutexLocker l(&_imp->activatedMutex);
        _imp->activated = false;
    }
} // deactivate

void
Node::activate(const std::list< Node* > & outputsToRestore,
               bool restoreAll,
               bool triggerRender)
{
    ///Only called by the main-thread
    assert( QThread::currentThread() == qApp->thread() );
    if (!_imp->liveInstance) {
        return;
    }
    
    ///No need to lock, guiInputs is only written to by the main-thread
    
    ///for all inputs, reconnect their output to this node
    for (U32 i = 0; i < _imp->inputs.size(); ++i) {
        if (_imp->inputs[i]) {
            _imp->inputs[i]->connectOutput(this);
        }
    }
    
    boost::shared_ptr<Node> thisShared = getApp()->getProject()->getNodePointer(this);
    
    
    ///Restore all outputs that was connected to this node
    for (std::map<Node*,int >::iterator it = _imp->deactivatedState.begin();
         it != _imp->deactivatedState.end(); ++it) {
        bool restore = false;
        if (restoreAll) {
            restore = true;
        } else {
            for (std::list<Node* >::const_iterator found = outputsToRestore.begin(); found != outputsToRestore.end(); ++found) {
                if (*found == it->first) {
                    restore = true;
                    break;
                }
            }
        }
        
        if (restore) {
            ///before connecting the outputs to this node, disconnect any link that has been made
            ///between the outputs by the user. This should normally never happen as the undo/redo
            ///stack follow always the same order.
            boost::shared_ptr<Node> outputHasInput = it->first->getInput(it->second);
            if (outputHasInput) {
                bool ok = getApp()->getProject()->disconnectNodes(outputHasInput.get(), it->first);
                assert(ok);
                (void)ok;
            }
            
            ///and connect the output to this node
            it->first->connectInput(thisShared, it->second);
        }
    }
    
    {
        QMutexLocker l(&_imp->activatedMutex);
        _imp->activated = true; //< flag it true before notifying the GUI because the gui rely on this flag (espcially the Viewer)
    }
    emit activated(triggerRender);
} // activate

boost::shared_ptr<KnobI>
Node::getKnobByName(const std::string & name) const
{
    ///MT-safe, never changes
    assert(_imp->knobsInitialized);
    
    return _imp->liveInstance->getKnobByName(name);
}

namespace {
    ///output is always RGBA with alpha = 255
    template<typename PIX,int maxValue>
    void
    renderPreview(const Natron::Image & srcImg,
                  int elemCount,
                  int *dstWidth,
                  int *dstHeight,
                  bool convertToSrgb,
                  unsigned int* dstPixels)
    {
        ///recompute it after the rescaling
        const RectI & srcBounds = srcImg.getBounds();
        double yZoomFactor = *dstHeight / (double)srcBounds.height();
        double xZoomFactor = *dstWidth / (double)srcBounds.width();
        double zoomFactor;
        
        if (xZoomFactor < yZoomFactor) {
            zoomFactor = xZoomFactor;
            *dstHeight = srcBounds.height() * zoomFactor;
        } else {
            zoomFactor = yZoomFactor;
            *dstWidth = srcBounds.width() * zoomFactor;
        }
        assert(elemCount >= 3);
        
        for (int i = 0; i < *dstHeight; ++i) {
            double y = (i - *dstHeight / 2.) / zoomFactor + (srcBounds.y1 + srcBounds.y2) / 2.;
            int yi = std::floor(y + 0.5);
            U32 *dst_pixels = dstPixels + *dstWidth * (*dstHeight - 1 - i);
            const PIX* src_pixels = (const PIX*)srcImg.pixelAt(srcBounds.x1, yi);
            if (!src_pixels) {
                // out of bounds
                for (int j = 0; j < *dstWidth; ++j) {
#ifndef __NATRON_WIN32__
                    dst_pixels[j] = toBGRA(0, 0, 0, 0);
#else
                    dst_pixels[j] = toBGRA(0, 0, 0, 255);
#endif
                }
            } else {
                for (int j = 0; j < *dstWidth; ++j) {
                    // bilinear interpolation is pointless when downscaling a lot, and this is a preview anyway.
                    // just use nearest neighbor
                    double x = (j - *dstWidth / 2.) / zoomFactor + (srcBounds.x1 + srcBounds.x2) / 2.;
                    int xi = std::floor(x + 0.5); // round to nearest
                    if ( (xi < 0) || ( xi >= (srcBounds.x2 - srcBounds.x1) ) ) {
#ifndef __NATRON_WIN32__
                        dst_pixels[j] = toBGRA(0, 0, 0, 0);
#else
                        dst_pixels[j] = toBGRA(0, 0, 0, 255);
#endif
                    } else {
                        float rFilt = src_pixels[xi * elemCount + 0] / (float)maxValue;
                        float gFilt = src_pixels[xi * elemCount + 1] / (float)maxValue;
                        float bFilt = src_pixels[xi * elemCount + 2] / (float)maxValue;
                        int r = Color::floatToInt<256>(convertToSrgb ? Natron::Color::to_func_srgb(rFilt) : rFilt);
                        int g = Color::floatToInt<256>(convertToSrgb ? Natron::Color::to_func_srgb(gFilt) : gFilt);
                        int b = Color::floatToInt<256>(convertToSrgb ? Natron::Color::to_func_srgb(bFilt) : bFilt);
                        dst_pixels[j] = toBGRA(r, g, b, 255);
                    }
                }
            }
        }
    } // renderPreview
}

class ComputingPreviewSetter_RAII
{
    Node::Implementation* _imp;
    
public:
    ComputingPreviewSetter_RAII(Node::Implementation* imp)
    : _imp(imp)
    {
        _imp->setComputingPreview(true);
    }
    
    ~ComputingPreviewSetter_RAII()
    {
        _imp->setComputingPreview(false);
        
        if (_imp->checkForExitPreview()) {
            return;
        }
    }
};

bool
Node::makePreviewImage(SequenceTime time,
                       int *width,
                       int *height,
                       unsigned int* buf)
{
    assert(_imp->knobsInitialized);
    if (!_imp->liveInstance) {
        return false;
    }
    
    if (_imp->checkForExitPreview()) {
        return false;
    }
    
    /// prevent 2 previews to occur at the same time since there's only 1 preview instance
    ComputingPreviewSetter_RAII computingPreviewRAII(_imp.get());
    
    RectD rod;
    bool isProjectFormat;
    RenderScale scale;
    scale.x = scale.y = 1.;
    U64 nodeHash = getHashValue();
    Natron::StatusEnum stat = _imp->liveInstance->getRegionOfDefinition_public(nodeHash,time, scale, 0, &rod, &isProjectFormat);
    if ( (stat == eStatusFailed) || rod.isNull() ) {
        return false;
    }
    assert( !rod.isNull() );
    double yZoomFactor = (double)*height / (double)rod.height();
    double xZoomFactor = (double)*width / (double)rod.width();
    double closestPowerOf2X = xZoomFactor >= 1 ? 1 : std::pow( 2,-std::ceil( std::log(xZoomFactor) / std::log(2.) ) );
    double closestPowerOf2Y = yZoomFactor >= 1 ? 1 : std::pow( 2,-std::ceil( std::log(yZoomFactor) / std::log(2.) ) );
    int closestPowerOf2 = std::max(closestPowerOf2X,closestPowerOf2Y);
    unsigned int mipMapLevel = std::min(std::log( (double)closestPowerOf2 ) / std::log(2.),5.);
    
    scale.x = Natron::Image::getScaleFromMipMapLevel(mipMapLevel);
    scale.y = scale.x;
    
    const double par = _imp->liveInstance->getPreferredAspectRatio();
    
    boost::shared_ptr<Image> img;
    RectI renderWindow;
    rod.toPixelEnclosing(mipMapLevel, par, &renderWindow);
    
    ParallelRenderArgsSetter frameRenderArgs(this,
                                             time,
                                             0, //< preview only renders view 0 (left)
                                             true,
                                             false,
                                             false,
                                             nodeHash,
                                             false,
                                             getApp()->getTimeLine().get());
    
    // Exceptions are caught because the program can run without a preview,
    // but any exception in renderROI is probably fatal.
    try {
        img = _imp->liveInstance->renderRoI( EffectInstance::RenderRoIArgs( time,
                                                                           scale,
                                                                           mipMapLevel,
                                                                           0, //< preview only renders view 0 (left)
                                                                           false,
                                                                           renderWindow,
                                                                           rod,
                                                                           Natron::eImageComponentRGB, //< preview is always rgb...
                                                                           getBitDepth() ) );
    } catch (...) {
        qDebug() << "Error: Cannot render preview";
        return false;
    }
    
    if (!img) {
        return false;
    }
    
    ImageComponentsEnum components = img->getComponents();
    int elemCount = getElementsCountForComponents(components);
    
    ///we convert only when input is Linear.
    //Rec709 and srGB is acceptable for preview
    bool convertToSrgb = getApp()->getDefaultColorSpaceForBitDepth( img->getBitDepth() ) == Natron::eViewerColorSpaceLinear;
    
    switch ( img->getBitDepth() ) {
        case Natron::eImageBitDepthByte: {
            renderPreview<unsigned char, 255>(*img, elemCount, width, height,convertToSrgb, buf);
            break;
        }
        case Natron::eImageBitDepthShort: {
            renderPreview<unsigned short, 65535>(*img, elemCount, width, height,convertToSrgb, buf);
            break;
        }
        case Natron::eImageBitDepthFloat: {
            renderPreview<float, 1>(*img, elemCount, width, height,convertToSrgb, buf);
            break;
        }
        case Natron::eImageBitDepthNone:
            break;
    }
    return true;
    
} // makePreviewImage

bool
Node::isInputNode() const
{
    ///MT-safe, never changes
    return _imp->liveInstance->isGenerator();
}

bool
Node::isOutputNode() const
{   ///MT-safe, never changes
    return _imp->liveInstance->isOutput();
}

bool
Node::isOpenFXNode() const
{
    ///MT-safe, never changes
    return _imp->liveInstance->isOpenFX();
}

bool
Node::isRotoNode() const
{
    ///Runs only in the main thread (checked by getName())
    ///Crude way to distinguish between Rotoscoping and Rotopainting nodes.
    return getPluginID() == PLUGINID_OFX_ROTO;
}

/**
 * @brief Returns true if the node is a rotopaint node
 **/
bool
Node::isRotoPaintingNode() const
{
    ///Runs only in the main thread (checked by getName())
    QString name = getPluginID().c_str();
    
    return name.contains("rotopaint",Qt::CaseInsensitive);
}

boost::shared_ptr<RotoContext>
Node::getRotoContext() const
{
    return _imp->rotoContext;
}

const std::vector<boost::shared_ptr<KnobI> > &
Node::getKnobs() const
{
    ///MT-safe from EffectInstance::getKnobs()
    return _imp->liveInstance->getKnobs();
}

void
Node::setKnobsFrozen(bool frozen)
{
    ///MT-safe from EffectInstance::setKnobsFrozen
    _imp->liveInstance->setKnobsFrozen(frozen);
    
    QMutexLocker l(&_imp->inputsMutex);
    for (U32 i = 0; i < _imp->inputs.size(); ++i) {
        if (_imp->inputs[i]) {
            _imp->inputs[i]->setKnobsFrozen(frozen);
        }
    }
}

std::string
Node::getPluginIconFilePath() const
{
    return _imp->plugin ? _imp->plugin->getIconFilePath().toStdString() : std::string();
}

std::string
Node::getPluginID() const
{
    ///MT-safe, never changes
    return _imp->liveInstance ? _imp->liveInstance->getPluginID() : std::string();
}

std::string
Node::getPluginLabel() const
{
    ///MT-safe, never changes
    return _imp->liveInstance->getPluginLabel();
}

std::string
Node::getDescription() const
{
    ///MT-safe, never changes
    return _imp->liveInstance->getDescription();
}

int
Node::getMaxInputCount() const
{
    ///MT-safe, never changes
    assert(_imp->liveInstance);
    
    return _imp->liveInstance->getMaxInputCount();
}

bool
Node::makePreviewByDefault() const
{
    ///MT-safe, never changes
    assert(_imp->liveInstance);
    
    return _imp->liveInstance->makePreviewByDefault();
}

void
Node::togglePreview()
{
    ///MT-safe from Knob
    assert(_imp->knobsInitialized);
    assert(_imp->previewEnabledKnob);
    _imp->previewEnabledKnob->setValue(!_imp->previewEnabledKnob->getValue(),0);
}

bool
Node::isPreviewEnabled() const
{
    ///MT-safe from EffectInstance
    if (!_imp->knobsInitialized) {
        qDebug() << "Node::isPreviewEnabled(): knobs not initialized (including previewEnabledKnob)";
    }
    if (_imp->previewEnabledKnob) {
        return _imp->previewEnabledKnob->getValue();
    }
    
    return false;
}

bool
Node::aborted() const
{
    ///MT-safe from EffectInstance
    assert(_imp->liveInstance);
    
    return _imp->liveInstance->aborted();
}

void
Node::setAborted(bool b)
{
    ///MT-safe from EffectInstance
    assert(_imp->liveInstance);
    _imp->liveInstance->setAborted(b);
    
    if (QThread::currentThread() == qApp->thread()) {
        ///The render thread is waiting for the main-thread to dequeue actions
        ///but the main-thread is waiting for the render thread to abort
        ///cancel the dequeuing
        QMutexLocker k(&_imp->nodeIsDequeuingMutex);
        _imp->nodeIsDequeuing = false;
        _imp->nodeIsDequeuingCond.wakeAll();
    }
    //
    //    QMutexLocker l(&_imp->inputsMutex);
    //
    //    for (U32 i = 0; i < _imp->inputs.size(); ++i) {
    //        if (_imp->inputs[i]) {
    //            _imp->inputs[i]->setAborted(b);
    //        }
    //    }
    
}

bool
Node::message(MessageTypeEnum type,
              const std::string & content) const
{
    ///If the node was aborted, don't transmit any message because we could cause a deadlock
    if ( _imp->liveInstance->aborted() ) {
        return false;
    }
    
    switch (type) {
        case eMessageTypeInfo:
            informationDialog(getName_mt_safe(), content);
            
            return true;
        case eMessageTypeWarning:
            warningDialog(getName_mt_safe(), content);
            
            return true;
        case eMessageTypeError:
            errorDialog(getName_mt_safe(), content);
            
            return true;
        case eMessageTypeQuestion:
            
            return questionDialog(getName_mt_safe(), content, false) == eStandardButtonYes;
        default:
            
            return false;
    }
}

void
Node::setPersistentMessage(MessageTypeEnum type,
                           const std::string & content)
{
    if ( !appPTR->isBackground() ) {
        //if the message is just an information, display a popup instead.
        if (type == eMessageTypeInfo) {
            message(type,content);
            
            return;
        }
        
        {
            QMutexLocker k(&_imp->persistentMessageMutex);
            
            QString message;
            message.append( getName_mt_safe().c_str() );
            if (type == eMessageTypeError) {
                message.append(" error: ");
                _imp->persistentMessageType = 1;
            } else if (type == eMessageTypeWarning) {
                message.append(" warning: ");
                _imp->persistentMessageType = 2;
            }
            message.append( content.c_str() );
            if (message == _imp->persistentMessage) {
                return;
            }
            _imp->persistentMessage = message;
        }
        emit persistentMessageChanged();
    } else {
        std::cout << "Persistent message" << std::endl;
        std::cout << content << std::endl;
    }
}

bool
Node::hasPersistentMessage() const
{
    QMutexLocker k(&_imp->persistentMessageMutex);
    return !_imp->persistentMessage.isEmpty();
}

void
Node::getPersistentMessage(QString* message,int* type) const
{
    QMutexLocker k(&_imp->persistentMessageMutex);
    *type = _imp->persistentMessageType;
    *message = _imp->persistentMessage;
}

void
Node::clearPersistentMessage(bool recurse)
{
    if ( !appPTR->isBackground() ) {
        {
            QMutexLocker k(&_imp->persistentMessageMutex);
            if (!_imp->persistentMessage.isEmpty()) {
                _imp->persistentMessage.clear();
                k.unlock();
                emit persistentMessageChanged();
            }
        }
    }
    
    if (recurse) {
        QMutexLocker l(&_imp->inputsMutex);
        ///No need to lock, guiInputs is only written to by the main-thread
        for (U32 i = 0; i < _imp->inputs.size(); ++i) {
            if (_imp->inputs[i]) {
                _imp->inputs[i]->clearPersistentMessage(true);
            }
        }
    }
    
}

void
Node::purgeAllInstancesCaches()
{
    ///Only called by the main-thread
    assert( QThread::currentThread() == qApp->thread() );
    assert(_imp->liveInstance);
    _imp->liveInstance->purgeCaches();
}

bool
Node::notifyInputNIsRendering(int inputNb)
{
    if (getApp()->isGuiFrozen()) {
        return false;
    }
    
    timeval now;
    
    
    gettimeofday(&now, 0);
    
    QMutexLocker l(&_imp->timersMutex);
    
    
    double t =  now.tv_sec  - _imp->lastInputNRenderStartedSlotCallTime.tv_sec +
    (now.tv_usec - _imp->lastInputNRenderStartedSlotCallTime.tv_usec) * 1e-6f;
    
    
    if (t > NATRON_RENDER_GRAPHS_HINTS_REFRESH_RATE_SECONDS) {
        
        _imp->lastInputNRenderStartedSlotCallTime = now;
        
        l.unlock();
        
        emit inputNIsRendering(inputNb);
        return true;
    }
    return false;
}

void
Node::notifyInputNIsFinishedRendering(int inputNb)
{
    emit inputNIsFinishedRendering(inputNb);
}

bool
Node::notifyRenderingStarted()
{
    if (getApp()->isGuiFrozen()) {
        return false;
    }
    
    timeval now;
    
    gettimeofday(&now, 0);
    
    QMutexLocker l(&_imp->timersMutex);
    
    double t =  now.tv_sec  - _imp->lastRenderStartedSlotCallTime.tv_sec +
    (now.tv_usec - _imp->lastRenderStartedSlotCallTime.tv_usec) * 1e-6f;
    
    if (t > NATRON_RENDER_GRAPHS_HINTS_REFRESH_RATE_SECONDS) {
        
        _imp->lastRenderStartedSlotCallTime = now;
        
        l.unlock();
        
        emit renderingStarted();
        return true;
    }
    return false;
}

void
Node::notifyRenderingEnded()
{
    emit renderingEnded();
}

void
Node::setOutputFilesForWriter(const std::string & pattern)
{
    assert(_imp->liveInstance);
    _imp->liveInstance->setOutputFilesForWriter(pattern);
}

void
Node::registerPluginMemory(size_t nBytes)
{
    {
        QMutexLocker l(&_imp->memoryUsedMutex);
        _imp->pluginInstanceMemoryUsed += nBytes;
    }
    emit pluginMemoryUsageChanged(nBytes);
}

void
Node::unregisterPluginMemory(size_t nBytes)
{
    {
        QMutexLocker l(&_imp->memoryUsedMutex);
        _imp->pluginInstanceMemoryUsed -= nBytes;
    }
    emit pluginMemoryUsageChanged(-nBytes);
}

QMutex &
Node::getRenderInstancesSharedMutex()
{
    return _imp->renderInstancesSharedMutex;
}

static void refreshPreviewsRecursivelyUpstreamInternal(int time,Node* node,std::list<Node*>& marked)
{
    if (std::find(marked.begin(), marked.end(), node) != marked.end()) {
        return;
    }

    if ( node->isPreviewEnabled() ) {
        node->refreshPreviewImage( time );
    }
    
    marked.push_back(node);
    
    std::vector<boost::shared_ptr<Node> > inputs = node->getInputs_copy();
    
    for (U32 i = 0; i < inputs.size(); ++i) {
        if (inputs[i]) {
            inputs[i]->refreshPreviewsRecursivelyUpstream(time);
        }
    }

}

void
Node::refreshPreviewsRecursivelyUpstream(int time)
{
    std::list<Node*> marked;
    refreshPreviewsRecursivelyUpstreamInternal(time,this,marked);
}

static void refreshPreviewsRecursivelyDownstreamInternal(int time,Node* node,std::list<Node*>& marked)
{
    if (std::find(marked.begin(), marked.end(), node) != marked.end()) {
        return;
    }
    
    if ( node->isPreviewEnabled() ) {
        node->refreshPreviewImage( time );
    }
    
    marked.push_back(node);
    
    std::list<Node*> outputs;
    node->getOutputs_mt_safe(outputs);
    for (std::list<Node*>::iterator it = outputs.begin(); it != outputs.end(); ++it) {
        assert(*it);
        (*it)->refreshPreviewsRecursivelyDownstream(time);
    }

}

void
Node::refreshPreviewsRecursivelyDownstream(int time)
{
    std::list<Node*> marked;
    refreshPreviewsRecursivelyDownstreamInternal(time,this,marked);
}

void
Node::onAllKnobsSlaved(bool isSlave,
                       KnobHolder* master)
{
    ///Only called by the main-thread
    assert( QThread::currentThread() == qApp->thread() );
    
    if (isSlave) {
        Natron::EffectInstance* effect = dynamic_cast<Natron::EffectInstance*>(master);
        assert(effect);
        boost::shared_ptr<Natron::Node> masterNode = effect->getNode();
        {
            QMutexLocker l(&_imp->masterNodeMutex);
            _imp->masterNode = masterNode;
        }
        QObject::connect( masterNode.get(), SIGNAL( deactivated(bool) ), this, SLOT( onMasterNodeDeactivated() ) );
        QObject::connect( masterNode.get(), SIGNAL( knobsAgeChanged(U64) ), this, SLOT( setKnobsAge(U64) ) );
        QObject::connect( masterNode.get(), SIGNAL( previewImageChanged(int) ), this, SLOT( refreshPreviewImage(int) ) );
    } else {
        QObject::disconnect( _imp->masterNode.get(), SIGNAL( deactivated(bool) ), this, SLOT( onMasterNodeDeactivated() ) );
        QObject::disconnect( _imp->masterNode.get(), SIGNAL( knobsAgeChanged(U64) ), this, SLOT( setKnobsAge(U64) ) );
        QObject::disconnect( _imp->masterNode.get(), SIGNAL( previewImageChanged(int) ), this, SLOT( refreshPreviewImage(int) ) );
        {
            QMutexLocker l(&_imp->masterNodeMutex);
            _imp->masterNode.reset();
        }
    }
    
    emit allKnobsSlaved(isSlave);
}

void
Node::onKnobSlaved(const boost::shared_ptr<KnobI> & knob,
                   int dimension,
                   bool isSlave,
                   KnobHolder* master)
{
    ///ignore the call if the node is a clone
    {
        QMutexLocker l(&_imp->masterNodeMutex);
        if (_imp->masterNode) {
            return;
        }
    }
    
    ///If the holder isn't an effect, ignore it too
    EffectInstance* isEffect = dynamic_cast<EffectInstance*>(master);
    
    if (!isEffect) {
        return;
    }
    boost::shared_ptr<Natron::Node> parentNode  = isEffect->getNode();
    bool changed = false;
    {
        QMutexLocker l(&_imp->masterNodeMutex);
        KnobLinkList::iterator found = _imp->nodeLinks.end();
        for (KnobLinkList::iterator it = _imp->nodeLinks.begin(); it != _imp->nodeLinks.end(); ++it) {
            if (it->masterNode == parentNode) {
                found = it;
                break;
            }
        }
        
        if ( found == _imp->nodeLinks.end() ) {
            if (!isSlave) {
                ///We want to unslave from the given node but the link didn't existed, just return
                return;
            } else {
                ///Add a new link
                KnobLink link;
                link.masterNode = parentNode;
                link.knob = knob;
                link.dimension = dimension;
                _imp->nodeLinks.push_back(link);
                changed = true;
            }
        } else if ( found != _imp->nodeLinks.end() ) {
            if (isSlave) {
                ///We want to slave to the given node but it already has a link on another parameter, just return
                return;
            } else {
                ///Remove the given link
                _imp->nodeLinks.erase(found);
                changed = true;
            }
        }
    }
    if (changed) {
        emit knobsLinksChanged();
    }
} // onKnobSlaved

void
Node::getKnobsLinks(std::list<Node::KnobLink> & links) const
{
    QMutexLocker l(&_imp->masterNodeMutex);
    
    links = _imp->nodeLinks;
}

void
Node::onMasterNodeDeactivated()
{
    ///Only called by the main-thread
    assert( QThread::currentThread() == qApp->thread() );
    _imp->liveInstance->unslaveAllKnobs();
}

boost::shared_ptr<Natron::Node>
Node::getMasterNode() const
{
    QMutexLocker l(&_imp->masterNodeMutex);
    
    return _imp->masterNode;
}

bool
Node::isSupportedComponent(int inputNb,
                           Natron::ImageComponentsEnum comp) const
{
    QMutexLocker l(&_imp->inputsMutex);
    
    if (inputNb >= 0) {
        assert( inputNb < (int)_imp->inputsComponents.size() );
        std::list<Natron::ImageComponentsEnum>::const_iterator found =
        std::find(_imp->inputsComponents[inputNb].begin(),_imp->inputsComponents[inputNb].end(),comp);
        
        return found != _imp->inputsComponents[inputNb].end();
    } else {
        assert(inputNb == -1);
        std::list<Natron::ImageComponentsEnum>::const_iterator found =
        std::find(_imp->outputComponents.begin(),_imp->outputComponents.end(),comp);
        
        return found != _imp->outputComponents.end();
    }
}

Natron::ImageComponentsEnum
Node::findClosestSupportedComponents(int inputNb,
                                     Natron::ImageComponentsEnum comp) const
{
    int compCount = getElementsCountForComponents(comp);
    QMutexLocker l(&_imp->inputsMutex);
    
    if (inputNb >= 0) {
        assert( inputNb < (int)_imp->inputsComponents.size() );
        
        
        const std::list<Natron::ImageComponentsEnum> & comps = _imp->inputsComponents[inputNb];
        if ( comps.empty() ) {
            return Natron::eImageComponentNone;
        }
        std::list<Natron::ImageComponentsEnum>::const_iterator closestComp = comps.end();
        for (std::list<Natron::ImageComponentsEnum>::const_iterator it = comps.begin(); it != comps.end(); ++it) {
            if ( closestComp == comps.end() ) {
                closestComp = it;
            } else {
                if ( std::abs(getElementsCountForComponents(*it) - compCount) <
                    std::abs(getElementsCountForComponents(*closestComp) - compCount) ) {
                    closestComp = it;
                }
            }
        }
        assert( closestComp != comps.end() );
        
        return *closestComp;
    } else {
        assert(inputNb == -1);
        const std::list<Natron::ImageComponentsEnum> & comps = _imp->outputComponents;
        if ( comps.empty() ) {
            return Natron::eImageComponentNone;
        }
        std::list<Natron::ImageComponentsEnum>::const_iterator closestComp = comps.end();
        for (std::list<Natron::ImageComponentsEnum>::const_iterator it = comps.begin(); it != comps.end(); ++it) {
            if ( closestComp == comps.end() ) {
                closestComp = it;
            } else {
                if ( std::abs(getElementsCountForComponents(*it) - compCount) <
                    std::abs(getElementsCountForComponents(*closestComp) - compCount) ) {
                    closestComp = it;
                }
            }
        }
        assert( closestComp != comps.end() );
        
        return *closestComp;
    }
}

int
Node::getMaskChannel(int inputNb) const
{
    std::map<int, boost::shared_ptr<Choice_Knob> >::const_iterator it = _imp->maskChannelKnob.find(inputNb);
    
    if ( it != _imp->maskChannelKnob.end() ) {
        return it->second->getValue() - 1;
    } else {
        return 3;
    }
}

bool
Node::isMaskEnabled(int inputNb) const
{
    std::map<int, boost::shared_ptr<Bool_Knob> >::const_iterator it = _imp->enableMaskKnob.find(inputNb);
    
    if ( it != _imp->enableMaskKnob.end() ) {
        return it->second->getValue();
    } else {
        return true;
    }
}

void
Node::lock(const boost::shared_ptr<Natron::Image> & image)
{
    
    QMutexLocker l(&_imp->imagesBeingRenderedMutex);
    std::list<boost::shared_ptr<Natron::Image> >::iterator it =
    std::find(_imp->imagesBeingRendered.begin(), _imp->imagesBeingRendered.end(), image);
    
    while ( it != _imp->imagesBeingRendered.end() ) {
        _imp->imageBeingRenderedCond.wait(&_imp->imagesBeingRenderedMutex);
        it = std::find(_imp->imagesBeingRendered.begin(), _imp->imagesBeingRendered.end(), image);
    }
    ///Okay the image is not used by any other thread, claim that we want to use it
    assert( it == _imp->imagesBeingRendered.end() );
    _imp->imagesBeingRendered.push_back(image);
}

bool
Node::tryLock(const boost::shared_ptr<Natron::Image> & image)
{
    
    QMutexLocker l(&_imp->imagesBeingRenderedMutex);
    std::list<boost::shared_ptr<Natron::Image> >::iterator it =
    std::find(_imp->imagesBeingRendered.begin(), _imp->imagesBeingRendered.end(), image);
    
    if ( it != _imp->imagesBeingRendered.end() ) {
        return false;
    }
    ///Okay the image is not used by any other thread, claim that we want to use it
    assert( it == _imp->imagesBeingRendered.end() );
    _imp->imagesBeingRendered.push_back(image);
    return true;
}

void
Node::unlock(const boost::shared_ptr<Natron::Image> & image)
{
    QMutexLocker l(&_imp->imagesBeingRenderedMutex);
    std::list<boost::shared_ptr<Natron::Image> >::iterator it =
    std::find(_imp->imagesBeingRendered.begin(), _imp->imagesBeingRendered.end(), image);
    ///The image must exist, otherwise this is a bug
    assert( it != _imp->imagesBeingRendered.end() );
    _imp->imagesBeingRendered.erase(it);
    ///Notify all waiting threads that we're finished
    _imp->imageBeingRenderedCond.wakeAll();
}

boost::shared_ptr<Natron::Image>
Node::getImageBeingRendered(int time,
                            unsigned int mipMapLevel,
                            int view)
{
    QMutexLocker l(&_imp->imagesBeingRenderedMutex);
    for (std::list<boost::shared_ptr<Natron::Image> >::iterator it = _imp->imagesBeingRendered.begin();
         it != _imp->imagesBeingRendered.end(); ++it) {
        const Natron::ImageKey &key = (*it)->getKey();
        if ( (key._view == view) && ((*it)->getMipMapLevel() == mipMapLevel) && (key._time == time) ) {
            return *it;
        }
    }
    return boost::shared_ptr<Natron::Image>();
}

void
Node::onInputChanged(int inputNb)
{
    assert( QThread::currentThread() == qApp->thread() );
    _imp->duringInputChangedAction = true;
    std::map<int, boost::shared_ptr<Bool_Knob> >::iterator it = _imp->enableMaskKnob.find(inputNb);
    if ( it != _imp->enableMaskKnob.end() ) {
        boost::shared_ptr<Node> inp = getInput(inputNb);
        it->second->setEvaluateOnChange(false);
        it->second->setValue(inp ? true : false, 0);
        it->second->setEvaluateOnChange(true);
    }
    _imp->liveInstance->onInputChanged(inputNb);
    _imp->duringInputChangedAction = false;
}

void
Node::onParentMultiInstanceInputChanged(int input)
{
    _imp->duringInputChangedAction = true;
    _imp->liveInstance->onInputChanged(input);
    _imp->duringInputChangedAction = false;
}


bool
Node::duringInputChangedAction() const
{
    assert( QThread::currentThread() == qApp->thread() );
    
    return _imp->duringInputChangedAction;
}

void
Node::computeFrameRangeForReader(const KnobI* fileKnob)
{
    int leftBound = INT_MIN;
    int rightBound = INT_MAX;
    ///Set the originalFrameRange parameter of the reader if it has one.
    boost::shared_ptr<KnobI> knob = getKnobByName("originalFrameRange");
    if (knob) {
        Int_Knob* originalFrameRange = dynamic_cast<Int_Knob*>(knob.get());
        if (originalFrameRange && originalFrameRange->getDimension() == 2) {
            
            const File_Knob* isFile = dynamic_cast<const File_Knob*>(fileKnob);
            assert(isFile);
            
            std::string pattern = isFile->getValue();
            getApp()->getProject()->canonicalizePath(pattern);
            SequenceParsing::SequenceFromPattern seq;
            SequenceParsing::filesListFromPattern(pattern, &seq);
            if (seq.empty() || seq.size() == 1) {
                leftBound = 1;
                rightBound = 1;
            } else if (seq.size() > 1) {
                leftBound = seq.begin()->first;
                rightBound = seq.rbegin()->first;
            }
            
            originalFrameRange->setValue(leftBound, 0);
            originalFrameRange->setValue(rightBound, 1);
            
        }
    }

}

void
Node::onEffectKnobValueChanged(KnobI* what,
                               Natron::ValueChangedReasonEnum reason)
{
    if (!what) {
        return;
    }
    for (std::map<int, boost::shared_ptr<Choice_Knob> >::iterator it = _imp->maskChannelKnob.begin(); it != _imp->maskChannelKnob.end(); ++it) {
        if (it->second.get() == what) {
            int index = it->second->getValue();
            std::map<int, boost::shared_ptr<Bool_Knob> >::iterator found = _imp->enableMaskKnob.find(it->first);
            if ( (index == 0) && found->second->isEnabled(0) ) {
                found->second->setValue(false, 0);
                found->second->setEnabled(0, false);
            } else if ( !found->second->isEnabled(0) ) {
                found->second->setEnabled(0, true);
                if ( getInput(it->first) ) {
                    found->second->setValue(true, 0);
                }
            }
            break;
        }
    }
    
    if ( what == _imp->previewEnabledKnob.get() ) {
        if ( (reason == Natron::eValueChangedReasonUserEdited) || (reason == Natron::eValueChangedReasonSlaveRefresh) ) {
            emit previewKnobToggled();
        }
    } else if ( ( what == _imp->disableNodeKnob.get() ) && !_imp->isMultiInstance && !_imp->multiInstanceParent ) {
        emit disabledKnobToggled( _imp->disableNodeKnob->getValue() );
        getApp()->redrawAllViewers();
    } else if ( what == _imp->nodeLabelKnob.get() ) {
        emit nodeExtraLabelChanged( _imp->nodeLabelKnob->getValue().c_str() );
    } else if (what->getName() == kNatronOfxParamStringSublabelName) {
        //special hack for the merge node and others so we can retrieve the sublabel and display it in the node's label
        String_Knob* strKnob = dynamic_cast<String_Knob*>(what);
        if (strKnob) {
            QString operation = strKnob->getValue().c_str();
            replaceCustomDataInlabel('(' + operation + ')');
        }
    } else if ( (what->getName() == kOfxImageEffectFileParamName) && _imp->liveInstance->isReader() ) {
        ///Refresh the preview automatically if the filename changed
        incrementKnobsAge(); //< since evaluate() is called after knobChanged we have to do this  by hand
        computePreviewImage( getApp()->getTimeLine()->currentFrame() );
        
        ///union the project frame range if not locked with the reader frame range
        bool isLocked = getApp()->getProject()->isFrameRangeLocked();
        if (!isLocked) {
            int leftBound = INT_MIN,rightBound = INT_MAX;
            _imp->liveInstance->getFrameRange_public(getHashValue(), &leftBound, &rightBound);
    
            if (leftBound != INT_MIN && rightBound != INT_MAX) {
                getApp()->getProject()->unionFrameRangeWith(leftBound, rightBound);
            }
        }
        
    } else if ( what == _imp->refreshInfoButton.get() ) {
        int maxinputs = getMaxInputCount();
        for (int i = 0; i < maxinputs; ++i) {
            std::string inputInfo = makeInfoForInput(i);
            _imp->inputFormats[i]->setValue(inputInfo, 0);
        }
        std::string outputInfo = makeInfoForInput(-1);
        _imp->outputFormat->setValue(outputInfo, 0);
 
    }
}

void
Node::replaceCustomDataInlabel(const QString & data)
{
    assert( QThread::currentThread() == qApp->thread() );
    
    QString label = _imp->nodeLabelKnob->getValue().c_str();
    ///Since the label is html encoded, find the text's start
    int foundFontTag = label.indexOf("<font");
    bool htmlPresent =  (foundFontTag != -1);
    ///we're sure this end tag is the one of the font tag
    QString endFont("\">");
    int endFontTag = label.indexOf(endFont,foundFontTag);
    QString customTagStart(NATRON_CUSTOM_HTML_TAG_START);
    QString customTagEnd(NATRON_CUSTOM_HTML_TAG_END);
    int foundNatronCustomDataTag = label.indexOf(customTagStart,endFontTag == -1 ? 0 : endFontTag);
    if (foundNatronCustomDataTag != -1) {
        ///remove the current custom data
        int foundNatronEndTag = label.indexOf(customTagEnd,foundNatronCustomDataTag);
        assert(foundNatronEndTag != -1);
        
        foundNatronEndTag += customTagEnd.size();
        label.remove(foundNatronCustomDataTag, foundNatronEndTag - foundNatronCustomDataTag);
    }
    
    int i = htmlPresent ? endFontTag + endFont.size() : 0;
    label.insert(i, customTagStart);
    label.insert(i + customTagStart.size(), data);
    label.insert(i + customTagStart.size() + data.size(), customTagEnd);
    _imp->nodeLabelKnob->setValue(label.toStdString(), 0);
}

bool
Node::isNodeDisabled() const
{
    return _imp->disableNodeKnob->getValue();
}

void
Node::setNodeDisabled(bool disabled)
{
    _imp->disableNodeKnob->setValue(disabled, 0);
}

void
Node::showKeyframesOnTimeline(bool emitSignal)
{
    assert( QThread::currentThread() == qApp->thread() );
    if ( _imp->keyframesDisplayedOnTimeline || appPTR->isBackground() ) {
        return;
    }
    _imp->keyframesDisplayedOnTimeline = true;
    std::list<SequenceTime> keys;
    getAllKnobsKeyframes(&keys);
    getApp()->getTimeLine()->addMultipleKeyframeIndicatorsAdded(keys, emitSignal);
}

void
Node::hideKeyframesFromTimeline(bool emitSignal)
{
    assert( QThread::currentThread() == qApp->thread() );
    if ( !_imp->keyframesDisplayedOnTimeline || appPTR->isBackground() ) {
        return;
    }
    _imp->keyframesDisplayedOnTimeline = false;
    std::list<SequenceTime> keys;
    getAllKnobsKeyframes(&keys);
    getApp()->getTimeLine()->removeMultipleKeyframeIndicator(keys, emitSignal);
}

bool
Node::areKeyframesVisibleOnTimeline() const
{
    assert( QThread::currentThread() == qApp->thread() );
    
    return _imp->keyframesDisplayedOnTimeline;
}

void
Node::getAllKnobsKeyframes(std::list<SequenceTime>* keyframes)
{
    assert(keyframes);
    const std::vector<boost::shared_ptr<KnobI> > & knobs = getKnobs();
    
    for (U32 i = 0; i < knobs.size(); ++i) {
        if ( knobs[i]->getIsSecret() || !knobs[i]->getIsPersistant()) {
            continue;
        }
        if (!knobs[i]->canAnimate()) {
            continue;
        }
        int dim = knobs[i]->getDimension();
        File_Knob* isFile = dynamic_cast<File_Knob*>( knobs[i].get() );
        if (isFile) {
            ///skip file knobs
            continue;
        }
        for (int j = 0; j < dim; ++j) {
            if (knobs[i]->canAnimate() && knobs[i]->isAnimated(j)) {
                KeyFrameSet kfs = knobs[i]->getCurve(j)->getKeyFrames_mt_safe();
                for (KeyFrameSet::iterator it = kfs.begin(); it != kfs.end(); ++it) {
                    keyframes->push_back( it->getTime() );
                }
            }
        }
    }
}

Natron::ImageBitDepthEnum
Node::getBitDepth() const
{
    bool foundShort = false;
    bool foundByte = false;
    
    for (std::list<ImageBitDepthEnum>::const_iterator it = _imp->supportedDepths.begin(); it != _imp->supportedDepths.end(); ++it) {
        switch (*it) {
            case Natron::eImageBitDepthFloat:
                
                return Natron::eImageBitDepthFloat;
                break;
            case Natron::eImageBitDepthByte:
                foundByte = true;
                break;
            case Natron::eImageBitDepthShort:
                foundShort = true;
                break;
            case Natron::eImageBitDepthNone:
                break;
        }
    }
    
    if (foundShort) {
        return Natron::eImageBitDepthShort;
    } else if (foundByte) {
        return Natron::eImageBitDepthByte;
    } else {
        ///The plug-in doesn't support any bitdepth, the program shouldn't even have reached here.
        assert(false);
        
        return Natron::eImageBitDepthNone;
    }
}

bool
Node::isSupportedBitDepth(Natron::ImageBitDepthEnum depth) const
{
    return std::find(_imp->supportedDepths.begin(), _imp->supportedDepths.end(), depth) != _imp->supportedDepths.end();
}

std::string
Node::getNodeExtraLabel() const
{
    return _imp->nodeLabelKnob->getValue();
}

bool
Node::hasSequentialOnlyNodeUpstream(std::string & nodeName) const
{
    ///Just take into account sequentiallity for writers
    if ( (_imp->liveInstance->getSequentialPreference() == Natron::eSequentialPreferenceOnlySequential) && _imp->liveInstance->isWriter() ) {
        nodeName = getName_mt_safe();
        
        return true;
    } else {
        
        
        QMutexLocker l(&_imp->inputsMutex);
        
        for (InputsV::iterator it = _imp->inputs.begin(); it != _imp->inputs.end(); ++it) {
            if ( (*it) && (*it)->hasSequentialOnlyNodeUpstream(nodeName) && (*it)->getLiveInstance()->isWriter() ) {
                nodeName = (*it)->getName();
                
                return true;
            }
        }
        
        return false;
    }
}

bool
Node::isTrackerNode() const
{
    return getPluginID() == PLUGINID_OFX_TRACKERPM;
}

void
Node::updateEffectLabelKnob(const QString & name)
{
    if (!_imp->liveInstance) {
        return;
    }
    boost::shared_ptr<KnobI> knob = getKnobByName(kNatronOfxParamStringSublabelName);
    String_Knob* strKnob = dynamic_cast<String_Knob*>( knob.get() );
    if (strKnob) {
        strKnob->setValue(name.toStdString(), 0);
    }
}

bool
Node::canOthersConnectToThisNode() const
{
    ///In debug mode only allow connections to Writer nodes
# ifdef DEBUG
    
    return dynamic_cast<const ViewerInstance*>(_imp->liveInstance) == NULL;
# else // !DEBUG
    return dynamic_cast<const ViewerInstance*>(_imp->liveInstance) == NULL/* && !_imp->liveInstance->isWriter()*/;
# endif // !DEBUG
}

void
Node::setParallelRenderArgs(int time,
                            int view,
                            bool isRenderUserInteraction,
                            bool isSequential,
                            bool canAbort,
                            U64 nodeHash,
                            bool canSetValue,
                            const TimeLine* timeline)
{
    std::list<Natron::Node*> marked;
    setParallelRenderArgsInternal(time, view, isRenderUserInteraction, isSequential, nodeHash,canAbort, canSetValue, timeline, marked);
}

void
Node::invalidateParallelRenderArgs()
{
    std::list<Natron::Node*> marked;
    invalidateParallelRenderArgsInternal(marked);
}

void
Node::invalidateParallelRenderArgsInternal(std::list<Natron::Node*>& markedNodes)
{
    ///If marked, we alredy set render args
    std::list<Natron::Node*>::iterator found = std::find(markedNodes.begin(), markedNodes.end(), this);
    if (found != markedNodes.end()) {
        return;
    }
    bool wasCanSetValueSet = _imp->liveInstance->invalidateParallelRenderArgs();
    
    bool mustDequeue ;
    {
        int nodeIsRendering;
        if (QThread::currentThread() != qApp->thread()) {
            
            if (!wasCanSetValueSet) {
                ///Decrement the node is rendering counter
                QMutexLocker k(&_imp->nodeIsRenderingMutex);
                --_imp->nodeIsRendering;
                assert(_imp->nodeIsRendering >= 0);
                nodeIsRendering = _imp->nodeIsRendering;
            } else {
                nodeIsRendering = 0;
            }
        } else {
            nodeIsRendering = 0;
        }
        
        mustDequeue = nodeIsRendering == 0 && !appPTR->isBackground();
    }
    
    if (mustDequeue) {
        
        ///Flag that the node is dequeuing.
        ///We don't wait here but in the setParallelRenderArgsInternal instead
        {
            QMutexLocker k(&_imp->nodeIsDequeuingMutex);
            _imp->nodeIsDequeuing = true;
        }
        emit mustDequeueActions();
    }
    
    ///mark this
    markedNodes.push_back(this);
    
    
    ///Call recursively
    int maxInpu = _imp->liveInstance->getMaxInputCount();
    for (int i = 0; i < maxInpu; ++i) {
        boost::shared_ptr<Node> input = getInput(i);
        if (input) {
            input->invalidateParallelRenderArgsInternal(markedNodes);
        }
    }
    
}

void
Node::setParallelRenderArgsInternal(int time,
                                    int view,
                                    bool isRenderUserInteraction,
                                    bool isSequential,
                                    U64 nodeHash,
                                    bool canAbort,
                                    bool canSetValue,
                                    const TimeLine* timeline,
                                    std::list<Natron::Node*>& markedNodes)
{
    ///If marked, we alredy set render args
    std::list<Natron::Node*>::iterator found = std::find(markedNodes.begin(), markedNodes.end(), this);
    if (found != markedNodes.end()) {
        return;
    }
    
    U64 rotoAge;
    if (_imp->rotoContext) {
        rotoAge = _imp->rotoContext->getAge();
    } else {
        rotoAge = 0;
    }
    
    _imp->liveInstance->setParallelRenderArgs(time, view, isRenderUserInteraction, isSequential, canAbort, nodeHash, rotoAge,canSetValue, timeline);
    
    
    ///Wait for the main-thread to be done dequeuing the connect actions queue
    if (QThread::currentThread() != qApp->thread()) {
        bool mustQuitProcessing;
        {
            QMutexLocker k(&_imp->mustQuitProcessingMutex);
            mustQuitProcessing = _imp->mustQuitProcessing;
        }
        QMutexLocker k(&_imp->nodeIsDequeuingMutex);
        while (_imp->nodeIsDequeuing && !aborted() && !mustQuitProcessing) {
            _imp->nodeIsDequeuingCond.wait(&_imp->nodeIsDequeuingMutex);
            {
                QMutexLocker k(&_imp->mustQuitProcessingMutex);
                mustQuitProcessing = _imp->mustQuitProcessing;
            }
        }
        if (!canSetValue) {
            ///Increment the node is rendering counter
            QMutexLocker nrLocker(&_imp->nodeIsRenderingMutex);
            ++_imp->nodeIsRendering;
        }
    }
    
    ///mark this
    markedNodes.push_back(this);
    
    
    ///Call recursively
    
    int maxInpu = _imp->liveInstance->getMaxInputCount();
    for (int i = 0; i < maxInpu; ++i) {
        boost::shared_ptr<Node> input = getInput(i);
        if (input) {
            input->setParallelRenderArgsInternal(time, view, isRenderUserInteraction, isSequential, input->getHashValue(),canAbort, canSetValue,  timeline, markedNodes);
            
        }
    }
    
}

bool
Node::isNodeRendering() const
{
    QMutexLocker k(&_imp->nodeIsRenderingMutex);
    return _imp->nodeIsRendering > 0;
}

void
Node::dequeueActions()
{
    assert(QThread::currentThread() == qApp->thread());
    
    if (_imp->liveInstance) {
        _imp->liveInstance->dequeueValuesSet();
    }
    
    std::list<ConnectInputAction> queue;
    {
        QMutexLocker k(&_imp->connectionQueueMutex);
        queue = _imp->connectionQueue;
        _imp->connectionQueue.clear();
    }
    
    
    for (std::list<ConnectInputAction>::iterator it = queue.begin(); it!= queue.end(); ++it) {
        
        switch (it->type) {
            case eInputActionConnect:
                connectInput(it->node, it->inputNb);
                break;
            case eInputActionDisconnect:
                disconnectInput(it->inputNb);
                break;
            case eInputActionReplace:
                replaceInput(it->node, it->inputNb);
                break;
        }

    }
    
    QMutexLocker k(&_imp->nodeIsDequeuingMutex);
    _imp->nodeIsDequeuing = false;
    _imp->nodeIsDequeuingCond.wakeAll();
}

bool
Node::shouldCacheOutput() const
{
    {
        //If true then we're in analysis, so we cache the input of the analysis effect
        
        QMutexLocker k(&_imp->outputsMutex);
        std::size_t sz = _imp->outputs.size();
        if (sz > 1) {
            ///The node is referenced multiple times below, cache it
            return true;
        } else {
            if (sz == 1) {
                //The output has its settings panel opened, meaning the user is actively editing the output, we want this node to be cached then.
                //If force caching or aggressive caching are enabled, we by-pass and cache it anyway.
                Node* output = _imp->outputs.front();
                
#pragma message WARN("Hack: Return true if the node is directly connected to a viewer, because otherwise we will never cache " \
"properly a graph which is linear (with only a single input tree). We need to rework the viewer cache to cache tiles and then " \
"we can remove this piece of code")
                ///+TEMPORARY
                ViewerInstance* isViewer = dynamic_cast<ViewerInstance*>(output->getLiveInstance());
                if (isViewer) {
                    int activeInputs[2];
                    isViewer->getActiveInputs(activeInputs[0], activeInputs[1]);
                    if (output->getInput(activeInputs[0]).get() == this ||
                        output->getInput(activeInputs[1]).get() == this) {
                        return true;
                    }
                }
                ///+TEMPORARY
                
                return output->isSettingsPanelOpened() ||
                _imp->liveInstance->doesTemporalClipAccess() ||
                _imp->liveInstance->getRecursionLevel() > 0 ||
                isForceCachingEnabled() ||
                appPTR->isAggressiveCachingEnabled() ||
                (isPreviewEnabled() && !appPTR->isBackground());
            } else {
                return isForceCachingEnabled() || appPTR->isAggressiveCachingEnabled();
            }
        }
    }
    
}

void
Node::setNodeGuiPointer(NodeGuiI* gui)
{
    assert(!_imp->guiPointer);
    assert(QThread::currentThread() == qApp->thread());
    _imp->guiPointer = gui;
}

bool
Node::isSettingsPanelOpened() const
{
    if (!_imp->guiPointer) {
        return false;
    }
    if (_imp->multiInstanceParent) {
        return _imp->multiInstanceParent->isSettingsPanelOpened();
    }
    {
        QMutexLocker k(&_imp->masterNodeMutex);
        if (_imp->masterNode) {
            return _imp->masterNode->isSettingsPanelOpened();
        }
        for (KnobLinkList::iterator it = _imp->nodeLinks.begin(); it != _imp->nodeLinks.end(); ++it) {
            if (it->masterNode->isSettingsPanelOpened()) {
                return true;
            }
        }
    }
    return _imp->guiPointer->isSettingsPanelOpened();
    
}

void
Node::restoreClipPreferencesRecursive(std::list<Natron::Node*>& markedNodes)
{
    std::list<Natron::Node*>::const_iterator found = std::find(markedNodes.begin(), markedNodes.end(), this);
    if (found != markedNodes.end()) {
        return;
    }
    
    InputsV inputs;
    
    {
        QMutexLocker k(&_imp->inputsMutex);
        inputs = _imp->inputs;
    }
    
    
    for (InputsV::iterator it = inputs.begin(); it != inputs.end(); ++it) {
        if ((*it)) {
            (*it)->restoreClipPreferencesRecursive(markedNodes);
        }
    }
    
    _imp->liveInstance->restoreClipPreferences();
    markedNodes.push_back(this);
    
}

//////////////////////////////////

InspectorNode::InspectorNode(AppInstance* app,
                             Natron::Plugin* plugin)
: Node(app,plugin)
, _inputsCount(1)
{
}

InspectorNode::~InspectorNode()
{
}

bool
InspectorNode::connectInput(const boost::shared_ptr<Node>& input,
                            int inputNumber)
{
    ///Only called by the main-thread
    assert( QThread::currentThread() == qApp->thread() );
    
    ///cannot connect more than 10 inputs.
    assert(inputNumber <= 10);
    
    assert(input);
    
    if ( !checkIfConnectingInputIsOk( input.get() ) ) {
        return false;
    }
    
    ///If the node 'input' is already to an input of the inspector, find it.
    ///If it has the same input number as what we want just return, otherwise
    ///disconnect it and continue as usual.
    int inputAlreadyConnected = inputIndex( input.get() );
    if (inputAlreadyConnected != -1) {
        if (inputAlreadyConnected == inputNumber) {
            return false;
        } else {
            disconnectInput(inputAlreadyConnected);
        }
    }
    
    /*Adding all empty edges so it creates at least the inputNB'th one.*/
    while (inputNumber >= _inputsCount) {
        ///this function might not succeed if we already have 10 inputs OR the last input is already empty
        addEmptyInput();
    }
  
    
    if ( !Node::connectInput(input, inputNumber) ) {
        
        computeHash();
    }
    tryAddEmptyInput();
    
    return true;
}

bool
InspectorNode::tryAddEmptyInput()
{
    ///Only called by the main-thread
    assert( QThread::currentThread() == qApp->thread() );
    
    
    ///if we already reached 10 inputs, just don't do anything
    if (_inputsCount <= 10) {
        if (_inputsCount > 0) {
            ///if there are already living inputs, look at the last one
            ///and if it is not connected, just don't add an input.
            ///Otherwise, add an empty input.
            if (getInput(_inputsCount - 1) != NULL) {
                addEmptyInput();
                
                return true;
            }
        } else {
            ///there'is no inputs yet, just add one.
            addEmptyInput();
            
            return true;
        }
    }
    
    return false;
}

void
InspectorNode::addEmptyInput()
{
    ///Only called by the main-thread
    assert( QThread::currentThread() == qApp->thread() );

    ++_inputsCount;
    initializeInputs();
}

void
InspectorNode::removeEmptyInputs()
{
    ///Only called by the main-thread
    assert( QThread::currentThread() == qApp->thread() );
    
    /*While there're NULL inputs at the tail of the map,remove them.
     Stops at the first non-NULL input.*/
    while (_inputsCount > 1) {
        if ( (getInput(_inputsCount - 1) == NULL) && (getInput(_inputsCount - 2) == NULL) ) {
            --_inputsCount;
            initializeInputs();
        } else {
            return;
        }
    }
}

int
InspectorNode::disconnectInput(int inputNumber)
{
    ///Only called by the main-thread
    assert( QThread::currentThread() == qApp->thread() );
    
    int ret = Node::disconnectInput(inputNumber);
    
    if (ret != -1) {
        removeEmptyInputs();

    }
    
    return ret;
}

int
InspectorNode::disconnectInput(Node* input)
{
    ///Only called by the main-thread
    assert( QThread::currentThread() == qApp->thread() );
    
    return disconnectInput( inputIndex( input ) );
}

void
InspectorNode::setActiveInputAndRefresh(int inputNb)
{
    if ( ( inputNb > (_inputsCount - 1) ) || (inputNb < 0) || (getInput(inputNb) == NULL) ) {
        return;
    }

    computeHash();
    emit inputChanged(inputNb);
    onInputChanged(inputNb);
    if ( isOutputNode() ) {
        Natron::OutputEffectInstance* oei = dynamic_cast<Natron::OutputEffectInstance*>( getLiveInstance() );
        assert(oei);
        if (oei) {
            oei->renderCurrentFrame(true);
        }
    }
}

int
InspectorNode::getPreferredInputForConnection()
{
    for (int i = 0; i < _inputsCount; ++i) {
        if (!getInput(i)) {
            return i;
        }
    }
    ///No free input, make a new one
    addEmptyInput();
    return _inputsCount - 1;
}

