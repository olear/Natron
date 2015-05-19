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

#include "OfxEffectInstance.h"

#include <locale>
#include <limits>
#include <stdexcept>

#include <QtCore/QDebug>
#include <QByteArray>
#include <QReadWriteLock>
#include <QPointF>


#include "Global/Macros.h"

#include <ofxhPluginCache.h>
#include <ofxhPluginAPICache.h>
#include <ofxhImageEffect.h>
#include <ofxhImageEffectAPI.h>
#include <ofxhHost.h>

#include <tuttle/ofxReadWrite.h>


#include "Engine/AppManager.h"
#include "Engine/OfxParamInstance.h"
#include "Engine/OfxClipInstance.h"
#include "Engine/OfxImageEffectInstance.h"
#include "Engine/OfxOverlayInteract.h"
#include "Engine/ViewerInstance.h"
#include "Engine/TimeLine.h"
#include "Engine/Project.h"
#include "Engine/KnobFile.h"
#include "Engine/KnobTypes.h"
#include "Engine/AppInstance.h"
#include "Engine/NodeSerialization.h"
#include "Engine/Node.h"
#include "Engine/Transform.h"

using namespace Natron;
using std::cout; using std::endl;

namespace  {
/**
 * @class This class is helpful to set thread-storage data on the clips of an effect
 * When destroyed, it is removed from the clips, ensuring they are removed.
 * It is to be instantiated right before calling the action that will need the per thread-storage
 * This way even if exceptions are thrown, clip thread-storage will be purged.
 *
 * All the info set on clip thread-storage are "cached" data that might be needed by a call of the OpenFX API which would
 * otherwise require a recursive action call, which is forbidden by the specification.
 * The more you pass parameters, the safer you are that the plug-in will not attempt recursive action calls but the more expensive
 * it is.
 **/
class ClipsThreadStorageSetter
{
public:
    ClipsThreadStorageSetter(OfxImageEffectInstance* effect,
                             bool skipDiscarding,     //< this is in case a recursive action is called
                             bool setView,
                             int view,
                             bool setMipmapLevel,
                             unsigned int mipMapLevel)
        : effect(effect)
          , skipDiscarding(skipDiscarding)
          , viewSet(setView)
          , mipMapLevelSet(setMipmapLevel)
    {

        if (setView) {
            effect->setClipsView(view);
        }
        if (setMipmapLevel) {
            effect->setClipsMipMapLevel(mipMapLevel);
        }
    }

    ~ClipsThreadStorageSetter()
    {
        if (!skipDiscarding) {

            if (viewSet) {
                effect->discardClipsView();
            }
            if (mipMapLevelSet) {
                effect->discardClipsMipMapLevel();
            }
            
        }
    }

private:
    OfxImageEffectInstance* effect;
    bool skipDiscarding;
    bool viewSet;
    bool mipMapLevelSet;
};
}

OfxEffectInstance::OfxEffectInstance(boost::shared_ptr<Natron::Node> node)
    : AbstractOfxEffectInstance(node)
      , _effect()
      , _natronPluginID()
      , _isOutput(false)
      , _penDown(false)
      , _overlayInteract(0)
      , _overlaySlaves()
      , _created(false)
      , _initialized(false)
      , _renderButton()
      , _renderSafety(EffectInstance::eRenderSafetyUnsafe)
      , _wasRenderSafetySet(false)
      , _renderSafetyLock(new QReadWriteLock)
      , _context(eContextNone)
      , _preferencesLock(new QReadWriteLock(QReadWriteLock::Recursive))
#ifdef DEBUG
      , _canSetValue()
#endif
{
    QObject::connect( this, SIGNAL( syncPrivateDataRequested() ), this, SLOT( onSyncPrivateDataRequested() ) );
}

void
OfxEffectInstance::createOfxImageEffectInstance(OFX::Host::ImageEffect::ImageEffectPlugin* plugin,
                                                const std::string & context,
                                                const NodeSerialization* serialization,
                                                 const std::list<boost::shared_ptr<KnobSerialization> >& paramValues,
                                                bool allowFileDialogs,
                                                bool disableRenderScaleSupport)
{
    /*Replicate of the code in OFX::Host::ImageEffect::ImageEffectPlugin::createInstance.
       We need to pass more parameters to the constructor . That means we cannot
       create it in the virtual function newInstance. Thus we create it before
       instanciating the OfxImageEffect. The problem is that calling OFX::Host::ImageEffect::ImageEffectPlugin::createInstance
       creates the OfxImageEffect and calls populate(). populate() will actually create all OfxClipInstance and OfxParamInstance.
       All these subclasses need a valid pointer to an this. Hence we need to set the pointer to this in
       OfxImageEffect BEFORE calling populate().
     */

    ///Only called from the main thread.
    assert( QThread::currentThread() == qApp->thread() );
    ContextEnum ctx = mapToContextEnum(context);

    if (disableRenderScaleSupport || ctx == eContextWriter) {
        setAsOutputNode();
        // Writers don't support render scale (full-resolution images are written to disk)
        setSupportsRenderScaleMaybe(eSupportsNo);
    }
    if (ctx == eContextReader) {
        // Tuttle readers don't support render scale as of 11/8/2014, but may crash (at least in debug configuration).
        // TuttleAVReader crashes on an assert in copy_and_convert_pixels( avSrcView, this->_dstView );
        std::string prefix("tuttle.");
        if ( !plugin->getIdentifier().compare(0, prefix.size(), prefix) ) {
            setSupportsRenderScaleMaybe(eSupportsNo);
        }
    }

    OFX::Host::PluginHandle* ph = plugin->getPluginHandle();
    assert( ph->getOfxPlugin() );
    assert(ph->getOfxPlugin()->mainEntry);
    (void)ph;
    OFX::Host::ImageEffect::Descriptor* desc = NULL;
    desc = plugin->getContext(context);
    if (!desc) {
        throw std::runtime_error(std::string("Failed to get description for OFX plugin in context ") + context);
    }
    _context = mapToContextEnum(context);
    
    std::string images;

    try {
        _effect = new Natron::OfxImageEffectInstance(plugin,*desc,context,false);
        assert(_effect);
        _effect->setOfxEffectInstance( dynamic_cast<OfxEffectInstance*>(this) );

        _natronPluginID = plugin->getIdentifier();
//        _natronPluginID = generateImageEffectClassName( _effect->getPlugin()->getIdentifier(),
//                                                        _effect->getPlugin()->getVersionMajor(),
//                                                        _effect->getPlugin()->getVersionMinor(),
//                                                        _effect->getDescriptor().getShortLabel(),
//                                                        _effect->getDescriptor().getLabel(),
//                                                        _effect->getDescriptor().getLongLabel(),
//                                                        _effect->getDescriptor().getPluginGrouping() );


        beginChanges();
        OfxStatus stat;
        {
            SET_CAN_SET_VALUE(true);
            
            stat = _effect->populate();
            
            
            initializeContextDependentParams();
            
            _effect->addParamsToTheirParents();
            
            if (stat != kOfxStatOK) {
                throw std::runtime_error("Error while populating the Ofx image effect");
            }
            assert( _effect->getPlugin() );
            assert( _effect->getPlugin()->getPluginHandle() );
            assert( _effect->getPlugin()->getPluginHandle()->getOfxPlugin() );
            assert(_effect->getPlugin()->getPluginHandle()->getOfxPlugin()->mainEntry);
            
            getNode()->createRotoContextConditionnally();
            
            getNode()->initializeInputs();
            getNode()->initializeKnobs( serialization ? *serialization : NodeSerialization( getApp() ), disableRenderScaleSupport ? 1 : 0 );
            
            ///before calling the createInstanceAction, load values
            if ( serialization && !serialization->isNull() ) {
                getNode()->loadKnobs(*serialization);
            }
            
            if (!paramValues.empty()) {
                getNode()->setValuesFromSerialization(paramValues);
            }
            
            //////////////////////////////////////////////////////
            ///////For READERS & WRITERS only we open an image file dialog
            if (allowFileDialogs && isReader() && !(serialization && !serialization->isNull()) && paramValues.empty()) {
                images = getApp()->openImageFileDialog();
            } else if (allowFileDialogs && isWriter() && !(serialization && !serialization->isNull())  && paramValues.empty()) {
                images = getApp()->saveImageFileDialog();
            }
            if (!images.empty()) {
                boost::shared_ptr<KnobSerialization> defaultFile = createDefaultValueForParam(kOfxImageEffectFileParamName, images);
                CreateNodeArgs::DefaultValuesList list;
                list.push_back(defaultFile);
                getNode()->setValuesFromSerialization(list);
            }
            //////////////////////////////////////////////////////
            
            
            {
                ///Take the preferences lock so that it cannot be modified throughout the action.
                QReadLocker preferencesLocker(_preferencesLock);
                stat = _effect->createInstanceAction();
            }
            _created = true;
            
            
        } // SET_CAN_SET_VALUE(true);
        
        
        if ( (stat != kOfxStatOK) && (stat != kOfxStatReplyDefault) ) {
            throw std::runtime_error("Could not create effect instance for plugin");
        }
        
        OfxPointD scaleOne;
        scaleOne.x = 1.;
        scaleOne.y = 1.;
        // Try to set renderscale support at plugin creation.
        // This is not always possible (e.g. if a param has a wrong value).
        if (supportsRenderScaleMaybe() == eSupportsMaybe) {
            // does the effect support renderscale?
            OfxRangeD range;
            range.min = 0;
            OfxStatus tdstat = _effect->getTimeDomainAction(range);
            if ( (tdstat == kOfxStatOK) || (tdstat == kOfxStatReplyDefault) ) {
                ClipsThreadStorageSetter clipSetter(effectInstance(),
                                                    false,
                                                    true, //< setView ?
                                                    0,
                                                    true,
                                                    0);
                double time = range.min;
                
                OfxRectD rod;
                OfxStatus rodstat = _effect->getRegionOfDefinitionAction(time, scaleOne, rod);
                if ( (rodstat == kOfxStatOK) || (rodstat == kOfxStatReplyDefault) ) {
                    OfxPointD scale;
                    scale.x = 0.5;
                    scale.y = 0.5;
                    rodstat = _effect->getRegionOfDefinitionAction(time, scale, rod);
                    if ( (rodstat == kOfxStatOK) || (rodstat == kOfxStatReplyDefault) ) {
                        setSupportsRenderScaleMaybe(eSupportsYes);
                    } else {
                        setSupportsRenderScaleMaybe(eSupportsNo);
                    }
                }
            }
        }
        
        
        
        // Check here that bitdepth and components given by getClipPreferences are supported by the effect.
        // If we don't, the following assert will crash at the beginning of EffectInstance::renderRoIInternal():
        // assert(isSupportedBitDepth(outputDepth) && isSupportedComponent(-1, outputComponents));
        // If a component/bitdepth is not supported (this is probably a plugin bug), use the closest one, but don't crash Natron.
        checkOFXClipPreferences_public(getApp()->getTimeLine()->currentFrame(), scaleOne, kOfxChangeUserEdited,true, false);
        
      
        // check that the plugin supports kOfxImageComponentRGBA for all the clips
        /*const std::vector<OFX::Host::ImageEffect::ClipDescriptor*> & clips = effectInstance()->getDescriptor().getClipsByOrder();
        for (U32 i = 0; i < clips.size(); ++i) {
            if ( (clips[i]->getProps().findStringPropValueIndex(kOfxImageEffectPropSupportedComponents, kOfxImageComponentRGBA) == -1)
                 && !clips[i]->isOptional() && !clips[i]->isMask() ) {
                appPTR->writeToOfxLog_mt_safe( QString( plugin->getDescriptor().getLabel().c_str() )
                                               + "RGBA components not supported by OFX plugin in context " + QString( context.c_str() ) );
                throw std::runtime_error(std::string("RGBA components not supported by OFX plugin in context ") + context);
            }
        }*/
    } catch (const std::exception & e) {
        qDebug() << "Error: Caught exception while creating OfxImageEffectInstance" << ": " << e.what();
        throw;
    } catch (...) {
        qDebug() << "Error: Caught exception while creating OfxImageEffectInstance";
        throw;
    }

    _initialized = true;
    
    ///Now that the instance is created, make sure instanceChangedActino is called for all extra default values
    ///that we set
    for (std::list<boost::shared_ptr<KnobSerialization> >::const_iterator it = paramValues.begin(); it != paramValues.end();++it) {
        boost::shared_ptr<KnobI> knob = getKnobByName((*it)->getName());
        assert(knob);
        for (int i = 0; i < knob->getDimension(); ++i) {
            knob->evaluateValueChange(i, Natron::eValueChangedReasonUserEdited);
        }
    }
    
    if (!images.empty()) {
        boost::shared_ptr<KnobI> fileNameKnob = getKnobByName(kOfxImageEffectFileParamName);
        if (fileNameKnob) {
            fileNameKnob->evaluateValueChange(0,Natron::eValueChangedReasonUserEdited);
        }
    }
    endChanges();
    
} // createOfxImageEffectInstance

OfxEffectInstance::~OfxEffectInstance()
{
    if (_overlayInteract) {
        delete _overlayInteract;
    }

    delete _effect;
    delete _renderSafetyLock;
    delete _preferencesLock;
}

bool
OfxEffectInstance::isEffectCreated() const
{
    return _created;
}

void
OfxEffectInstance::initializeContextDependentParams()
{
    assert(_context != eContextNone);
    if ( isWriter() ) {
        _renderButton = Natron::createKnob<Button_Knob>(this, "Render");
        _renderButton->setHintToolTip("Starts rendering the specified frame range.");
        _renderButton->setAsRenderButton();
    }
}

std::string
OfxEffectInstance::getDescription() const
{
    assert(_context != eContextNone);
    if ( effectInstance() ) {
        return effectInstance()->getProps().getStringProperty(kOfxPropPluginDescription);
    } else {
        return "";
    }
}

void
OfxEffectInstance::tryInitializeOverlayInteracts()
{
    assert(_context != eContextNone);
    /*create overlay instance if any*/
    OfxPluginEntryPoint *overlayEntryPoint = _effect->getOverlayInteractMainEntry();
    if (overlayEntryPoint) {
        _overlayInteract = new OfxOverlayInteract(*_effect,8,true);
        RenderScale s;
        effectInstance()->getRenderScaleRecursive(s.x, s.y);

        {
            ClipsThreadStorageSetter clipSetter(effectInstance(),
                                                false,
                                                true, //< setView ?
                                                0,
                                                true,
                                                0);


            {
                SET_CAN_SET_VALUE(true);
                ///Take the preferences lock so that it cannot be modified throughout the action.
                QReadLocker preferencesLocker(_preferencesLock);
                _overlayInteract->createInstanceAction();
            }
        }
        
        ///Fetch all parameters that are overlay slave
        std::vector<std::string> slaveParams;
        _overlayInteract->getSlaveToParam(slaveParams);
        for (U32 i = 0; i < slaveParams.size(); ++i) {
            boost::shared_ptr<KnobI> param = getKnobByName(slaveParams[i]);
            assert(param);
            _overlaySlaves.push_back((void*)param.get());
        }
        
        getApp()->redrawAllViewers();
    }
    
    
    
    ///for each param, if it has a valid custom interact, create it
    const std::list<OFX::Host::Param::Instance*> & params = effectInstance()->getParamList();
    for (std::list<OFX::Host::Param::Instance*>::const_iterator it = params.begin(); it != params.end(); ++it) {
        OfxParamToKnob* paramToKnob = dynamic_cast<OfxParamToKnob*>(*it);
        assert(paramToKnob);
        OFX::Host::Interact::Descriptor & interactDesc = paramToKnob->getInteractDesc();
        if (interactDesc.getState() == OFX::Host::Interact::eDescribed) {
            boost::shared_ptr<KnobI> knob = paramToKnob->getKnob();
            boost::shared_ptr<OfxParamOverlayInteract> overlay( new OfxParamOverlayInteract( knob.get(),interactDesc,
                                                                                             effectInstance()->getHandle() ) );

            {
                SET_CAN_SET_VALUE(true);
                ///Take the preferences lock so that it cannot be modified throughout the action.
                QReadLocker preferencesLocker(_preferencesLock);
                overlay->createInstanceAction();
            }
            knob->setCustomInteract(overlay);
        }
    }
}

bool
OfxEffectInstance::isOutput() const
{
    assert(_context != eContextNone);

    return _isOutput;
}

bool
OfxEffectInstance::isGenerator() const
{
#if 0
    assert( effectInstance() );
    const std::set<std::string> & contexts = effectInstance()->getPlugin()->getContexts();
    std::set<std::string>::const_iterator foundGenerator = contexts.find(kOfxImageEffectContextGenerator);
    std::set<std::string>::const_iterator foundReader = contexts.find(kOfxImageEffectContextReader);
    if ( ( foundGenerator != contexts.end() ) || ( foundReader != contexts.end() ) ) {
        return true;
    }

    return false;
#else
    assert(_context != eContextNone);

    return _context == eContextGenerator || _context == eContextReader;
#endif
}

bool
OfxEffectInstance::isReader() const
{
#if 0
    assert( effectInstance() );
    const std::set<std::string> & contexts = effectInstance()->getPlugin()->getContexts();
    std::set<std::string>::const_iterator foundReader = contexts.find(kOfxImageEffectContextReader);
    if ( foundReader != contexts.end() ) {
        return true;
    }

    return false;
#else
    assert(_context != eContextNone);

    return _context == eContextReader;
#endif
}

bool
OfxEffectInstance::isWriter() const
{
#if 0
    assert(_context != eContextNone);
    assert( effectInstance() );
    const std::set<std::string> & contexts = effectInstance()->getPlugin()->getContexts();
    std::set<std::string>::const_iterator foundWriter = contexts.find(kOfxImageEffectContextWriter);
    if ( foundWriter != contexts.end() ) {
        return true;
    }

    return false;
#else
    assert(_context != eContextNone);

    return _context == eContextWriter;
#endif
}

bool
OfxEffectInstance::isGeneratorAndFilter() const
{
    assert(_context != eContextNone);
    const std::set<std::string> & contexts = effectInstance()->getPlugin()->getContexts();
    std::set<std::string>::const_iterator foundGenerator = contexts.find(kOfxImageEffectContextGenerator);
    std::set<std::string>::const_iterator foundGeneral = contexts.find(kOfxImageEffectContextGeneral);

    return foundGenerator != contexts.end() && foundGeneral != contexts.end();
}

/*group is a string as such:
   Toto/Superplugins/blabla
   This functions extracts the all parts of such a grouping, e.g in this case
   it would return [Toto,Superplugins,blabla].*/
static
QStringList
ofxExtractAllPartsOfGrouping(const QString & pluginIdentifier,
                             int /*versionMajor*/,
                             int /*versionMinor*/,
                             const QString & /*pluginLabel*/,
                             const QString & str)
{
    QString s(str);

    s.replace( QChar('\\'),QChar('/') );

    QStringList out;
    if ( pluginIdentifier.startsWith("com.genarts.sapphire.") || s.startsWith("Sapphire ") || str.startsWith(" Sapphire ") ) {
        out.push_back("Sapphire");

    } else if ( pluginIdentifier.startsWith("com.genarts.monsters.") || s.startsWith("Monsters ") || str.startsWith(" Monsters ") ) {
        out.push_back("Monsters");

    } else if (pluginIdentifier == "uk.co.thefoundry.keylight.keylight") {
        s = PLUGIN_GROUP_KEYER;

    } else if (pluginIdentifier == "uk.co.thefoundry.noisetools.denoise") {
        s = PLUGIN_GROUP_FILTER;

    } else if ( (pluginIdentifier == "tuttle.avreader") ||
               (pluginIdentifier == "tuttle.avwriter") ||
               (pluginIdentifier == "tuttle.dpxwriter") ||
               (pluginIdentifier == "tuttle.exrreader") ||
               (pluginIdentifier == "tuttle.exrwriter") ||
               (pluginIdentifier == "tuttle.imagemagickreader") ||
               (pluginIdentifier == "tuttle.jpeg2000reader") ||
               (pluginIdentifier == "tuttle.jpeg2000writer") ||
               (pluginIdentifier == "tuttle.jpegreader") ||
               (pluginIdentifier == "tuttle.jpegwriter") ||
               (pluginIdentifier == "tuttle.oiioreader") ||
               (pluginIdentifier == "tuttle.oiiowriter") ||
               (pluginIdentifier == "tuttle.pngreader") ||
               (pluginIdentifier == "tuttle.pngwriter") ||
               (pluginIdentifier == "tuttle.rawreader") ||
               (pluginIdentifier == "tuttle.turbojpegreader") ||
               (pluginIdentifier == "tuttle.turbojpegwriter") ) {
        out.push_back(PLUGIN_GROUP_IMAGE);
        if ( pluginIdentifier.endsWith("reader") ) {
            s = PLUGIN_GROUP_IMAGE_READERS;
        } else {
            s = PLUGIN_GROUP_IMAGE_WRITERS;
        }

    } else if ( (pluginIdentifier == "tuttle.checkerboard") ||
               (pluginIdentifier == "tuttle.colorbars") ||
               (pluginIdentifier == "tuttle.colorcube") || // TuttleColorCube
               (pluginIdentifier == "tuttle.colorgradient") ||
               (pluginIdentifier == "tuttle.colorwheel") ||
               (pluginIdentifier == "tuttle.constant") ||
               (pluginIdentifier == "tuttle.inputbuffer") ||
               (pluginIdentifier == "tuttle.outputbuffer") ||
               (pluginIdentifier == "tuttle.ramp") ||
               (pluginIdentifier == "tuttle.seexpr") ) {
        s = PLUGIN_GROUP_IMAGE;

    } else if ( (pluginIdentifier == "tuttle.bitdepth") ||
               (pluginIdentifier == "tuttle.colorgradation") ||
               (pluginIdentifier == "tuttle.colorspace") ||
               (pluginIdentifier == "tuttle.colorsuppress") ||
               (pluginIdentifier == "tuttle.colortransfer") ||
               (pluginIdentifier == "tuttle.colortransform") ||
               (pluginIdentifier == "tuttle.ctl") ||
               (pluginIdentifier == "tuttle.invert") ||
               (pluginIdentifier == "tuttle.lut") ||
               (pluginIdentifier == "tuttle.normalize") ) {
        s = PLUGIN_GROUP_COLOR;

    } else if ( (pluginIdentifier == "tuttle.ocio.colorspace") ||
               (pluginIdentifier == "tuttle.ocio.lut") ) {
        out.push_back(PLUGIN_GROUP_COLOR);
        s = "OCIO";

    } else if ( (pluginIdentifier == "tuttle.gamma") ||
               (pluginIdentifier == "tuttle.mathoperator") ) {
        out.push_back(PLUGIN_GROUP_COLOR);
        s = "Math";

    } else if ( (pluginIdentifier == "tuttle.channelshuffle") ) {
        s = PLUGIN_GROUP_CHANNEL;

    } else if ( (pluginIdentifier == "tuttle.component") ||
               (pluginIdentifier == "tuttle.fade") ||
               (pluginIdentifier == "tuttle.merge") ) {
        s = PLUGIN_GROUP_MERGE;

    } else if ( (pluginIdentifier == "tuttle.anisotropicdiffusion") ||
                (pluginIdentifier == "tuttle.anisotropictensors") ||
                (pluginIdentifier == "tuttle.blur") ||
                (pluginIdentifier == "tuttle.convolution") ||
                (pluginIdentifier == "tuttle.floodfill") ||
                (pluginIdentifier == "tuttle.localmaxima") ||
                (pluginIdentifier == "tuttle.nlmdenoiser") ||
                (pluginIdentifier == "tuttle.sobel") ||
                (pluginIdentifier == "tuttle.thinning") ) {
        s = PLUGIN_GROUP_FILTER;

    } else if ( (pluginIdentifier == "tuttle.crop") ||
               (pluginIdentifier == "tuttle.flip") ||
               (pluginIdentifier == "tuttle.lensdistort") ||
               (pluginIdentifier == "tuttle.move2d") ||
               (pluginIdentifier == "tuttle.pinning") ||
               (pluginIdentifier == "tuttle.pushpixel") ||
               (pluginIdentifier == "tuttle.resize") ||
               (pluginIdentifier == "tuttle.swscale") ||
               (pluginIdentifier == "tuttle.warp") ) {
        s = PLUGIN_GROUP_TRANSFORM;

    } else if ( (pluginIdentifier == "tuttle.timeshift") ) {
        s = PLUGIN_GROUP_TIME;

    } else if ( (pluginIdentifier == "tuttle.text") ) {
        s = PLUGIN_GROUP_PAINT;

    } else if ( (pluginIdentifier == "tuttle.basickeyer") ||
                (pluginIdentifier == "tuttle.colorspacekeyer") ||
                (pluginIdentifier == "tuttle.histogramkeyer") ||
                (pluginIdentifier == "tuttle.idkeyer") ) {
        s = PLUGIN_GROUP_KEYER;

    } else if ( (pluginIdentifier == "tuttle.colorCube") || // TuttleColorCubeViewer
               (pluginIdentifier == "tuttle.colorcubeviewer") ||
               (pluginIdentifier == "tuttle.diff") ||
               (pluginIdentifier == "tuttle.dummy") ||
               (pluginIdentifier == "tuttle.histogram") ||
               (pluginIdentifier == "tuttle.imagestatistics") ) {
        s = PLUGIN_GROUP_OTHER;
        
    } else if ( (pluginIdentifier == "tuttle.debugimageeffectapi") ) {
        out.push_back(PLUGIN_GROUP_OTHER);
        s = "Test";
    }

    // The following plugins are pretty much useless for use within Natron, keep them in the Tuttle group:
    /*
       (pluginIdentifier == "tuttle.print") ||
       (pluginIdentifier == "tuttle.viewer") ||
     */
    return out + s.split('/');
} // ofxExtractAllPartsOfGrouping

QStringList
AbstractOfxEffectInstance::makePluginGrouping(const std::string & pluginIdentifier,
                                              int versionMajor,
                                              int versionMinor,
                                              const std::string & pluginLabel,
                                              const std::string & grouping)
{
    //printf("%s,%s\n",pluginLabel.c_str(),grouping.c_str());
    return ofxExtractAllPartsOfGrouping( pluginIdentifier.c_str(), versionMajor, versionMinor, pluginLabel.c_str(),grouping.c_str() );
}

std::string
AbstractOfxEffectInstance::makePluginLabel(const std::string & shortLabel,
                                           const std::string & label,
                                           const std::string & longLabel)
{
    std::string labelToUse = label;

    if ( labelToUse.empty() ) {
        labelToUse = shortLabel;
    }
    if ( labelToUse.empty() ) {
        labelToUse = longLabel;
    }

    return labelToUse;
}


std::string
OfxEffectInstance::getPluginID() const
{
    assert(_context != eContextNone);

    return _natronPluginID;
}

std::string
OfxEffectInstance::getPluginLabel() const
{
    assert(_context != eContextNone);
    assert(_effect);

    return makePluginLabel( _effect->getDescriptor().getShortLabel(),
                            _effect->getDescriptor().getLabel(),
                            _effect->getDescriptor().getLongLabel() );
}

void
OfxEffectInstance::getPluginGrouping(std::list<std::string>* grouping) const
{
    assert(_context != eContextNone);
    std::string groupStr = effectInstance()->getPluginGrouping();
    std::string label = getPluginLabel();
    const OFX::Host::ImageEffect::ImageEffectPlugin *p = effectInstance()->getPlugin();
    QStringList groups = ofxExtractAllPartsOfGrouping( p->getIdentifier().c_str(), p->getVersionMajor(), p->getVersionMinor(), label.c_str(), groupStr.c_str() );
    for (int i = 0; i < groups.size(); ++i) {
        grouping->push_back( groups[i].toStdString() );
    }
}

std::string
OfxEffectInstance::getInputLabel(int inputNb) const
{
    assert(_context != eContextNone);

    MappedInputV copy = inputClipsCopyWithoutOutput();
    if ( inputNb < (int)copy.size() ) {
        return copy[copy.size() - 1 - inputNb]->getShortLabel();
    } else {
        return EffectInstance::getInputLabel(inputNb);
    }
}

OfxEffectInstance::MappedInputV
OfxEffectInstance::inputClipsCopyWithoutOutput() const
{
    assert(_context != eContextNone);
    assert( effectInstance() );
    const std::vector<OFX::Host::ImageEffect::ClipDescriptor*> & clips = effectInstance()->getDescriptor().getClipsByOrder();
    MappedInputV copy;
    for (U32 i = 0; i < clips.size(); ++i) {
        assert(clips[i]);
        if (clips[i]->getShortLabel() != kOfxImageEffectOutputClipName) {
            copy.push_back(clips[i]);
            // cout << "Clip[" << i << "] = " << clips[i]->getShortLabel() << endl;
        }
    }

    return copy;
}

OfxClipInstance*
OfxEffectInstance::getClipCorrespondingToInput(int inputNo) const
{
    assert(_context != eContextNone);
    OfxEffectInstance::MappedInputV clips = inputClipsCopyWithoutOutput();
    assert( inputNo < (int)clips.size() );
    OFX::Host::ImageEffect::ClipInstance* clip = _effect->getClip( clips[clips.size() - 1 - inputNo]->getName() );
    assert(clip);

    return dynamic_cast<OfxClipInstance*>(clip);
}

int
OfxEffectInstance::getMaxInputCount() const
{
    assert(_context != eContextNone);
    const std::string & context = effectInstance()->getContext();
    if ( (context == kOfxImageEffectContextReader) ||
         ( context == kOfxImageEffectContextGenerator) ) {
        return 0;
    } else {
        assert( effectInstance() );
        int totalClips = effectInstance()->getDescriptor().getClips().size();

        return totalClips > 0  ?  totalClips - 1 : 0;
    }
}

bool
OfxEffectInstance::isInputOptional(int inputNb) const
{
    assert(_context != eContextNone);
    MappedInputV inputs = inputClipsCopyWithoutOutput();
    assert( inputNb < (int)inputs.size() );
    if ( inputs[inputs.size() - 1 - inputNb]->isOptional() ) {
        return true;
    } else {
        if ( isInputRotoBrush(inputNb) ) {
            return true;
        }
    }

    return false;
}

bool
OfxEffectInstance::isInputMask(int inputNb) const
{
    assert(_context != eContextNone);
    MappedInputV inputs = inputClipsCopyWithoutOutput();
    assert( inputNb < (int)inputs.size() );

    return inputs[inputs.size() - 1 - inputNb]->isMask();
}

bool
OfxEffectInstance::isInputRotoBrush(int inputNb) const
{
    assert(_context != eContextNone);
    MappedInputV inputs = inputClipsCopyWithoutOutput();
    if ( inputNb >= (int)inputs.size() ) {
        return false;
    }

    ///Maybe too crude ? Not like many plug-ins use the paint context except Natron's roto node.
    return inputs[inputs.size() - 1 - inputNb]->getName() == "Roto" && getNode()->isRotoNode();
}

int
OfxEffectInstance::getRotoBrushInputIndex() const
{
    assert(_context != eContextNone);
    MappedInputV inputs = inputClipsCopyWithoutOutput();
    for (U32 i = 0; i < inputs.size(); ++i) {
        if (inputs[i]->getName() == "Roto") {
            return inputs.size() - 1 - i;
        }
    }
    return -1;
}

void
OfxEffectInstance::onInputChanged(int inputNo)
{
    
    if (getApp()->getProject()->isLoadingProject()) {
        return;
    }
    assert(_context != eContextNone);
    OfxClipInstance* clip = getClipCorrespondingToInput(inputNo);
    assert(clip);
    double time = getApp()->getTimeLine()->currentFrame();
    RenderScale s;
    s.x = s.y = 1.;
    
    
    /**
     * The plug-in might call getImage, set a valid thread storage on the tree.
     **/
    ParallelRenderArgsSetter frameRenderArgs(_node.get(),
                                                   time,
                                                   0 /*view*/,
                                                   true,
                                                   false,
                                                   false,
                                                   getHash(),
                                                   true,
                                                   getApp()->getTimeLine().get());
    
    ///Don't do clip preferences while loading a project, they will be refreshed globally once the project is loaded.
    
    ///if all non optional clips are connected, call getClipPrefs
    ///The clip preferences action is never called until all non optional clips have been attached to the plugin.
    if (_effect->areAllNonOptionalClipsConnected()) {
        
        ///Render scale support might not have been set already because getRegionOfDefinition could have failed until all non optional inputs were connected
        if (supportsRenderScaleMaybe() == eSupportsMaybe) {
            OfxRectD rod;
            OfxPointD scaleOne;
            scaleOne.x = scaleOne.y = 1.;
            OfxStatus rodstat = _effect->getRegionOfDefinitionAction(time, scaleOne, rod);
            if ( (rodstat == kOfxStatOK) || (rodstat == kOfxStatReplyDefault) ) {
                OfxPointD scale;
                scale.x = 0.5;
                scale.y = 0.5;
                rodstat = _effect->getRegionOfDefinitionAction(time, scale, rod);
                if ( (rodstat == kOfxStatOK) || (rodstat == kOfxStatReplyDefault) ) {
                    setSupportsRenderScaleMaybe(eSupportsYes);
                } else {
                    setSupportsRenderScaleMaybe(eSupportsNo);
                }
            }

        }
        if ( !getApp()->getProject()->isLoadingProject() ) {
            checkOFXClipPreferences_public(time,s,kOfxChangeUserEdited,true, true);
        }
    }
    
    {
        RECURSIVE_ACTION();
        SET_CAN_SET_VALUE(true);
        ClipsThreadStorageSetter clipSetter(effectInstance(),
                                            false,
                                            true, //< setView ?
                                            0 /*view*/,
                                            true, //< setmipmaplevel?
                                            0);

        _effect->beginInstanceChangedAction(kOfxChangeUserEdited);
        _effect->clipInstanceChangedAction(clip->getName(), kOfxChangeUserEdited, time, s);
        _effect->endInstanceChangedAction(kOfxChangeUserEdited);
    }

}

/** @brief map a std::string to a context */
OfxEffectInstance::ContextEnum
OfxEffectInstance::mapToContextEnum(const std::string &s)
{
    if (s == kOfxImageEffectContextGenerator) {
        return eContextGenerator;
    }
    if (s == kOfxImageEffectContextFilter) {
        return eContextFilter;
    }
    if (s == kOfxImageEffectContextTransition) {
        return eContextTransition;
    }
    if (s == kOfxImageEffectContextPaint) {
        return eContextPaint;
    }
    if (s == kOfxImageEffectContextGeneral) {
        return eContextGeneral;
    }
    if (s == kOfxImageEffectContextRetimer) {
        return eContextRetimer;
    }
    if (s == kOfxImageEffectContextReader) {
        return eContextReader;
    }
    if (s == kOfxImageEffectContextWriter) {
        return eContextWriter;
    }
    qDebug() << "OfxEffectInstance::mapToContextEnum: Unknown image effect context '" << s.c_str() << "'";
    throw std::invalid_argument(s);
}

/**
 * @brief The purpose of this function is to allow Natron to modify slightly the values returned in the getClipPreferencesAction
 * by the plugin so that we can minimize the amount of Natron::Image::convertToFormat calls.
 **/
static void
clipPrefsProxy(OfxEffectInstance* self,
               double time,
               std::map<OfxClipInstance*,OfxImageEffectInstance::ClipPrefs>& clipPrefs,
               OfxImageEffectInstance::EffectPrefs& effectPrefs,
               std::list<OfxClipInstance*>& changedClips)
{
    ///We remap all the input clips components to be the same as the output clip, except for the masks.
    OfxClipInstance* outputClip = dynamic_cast<OfxClipInstance*>(self->effectInstance()->getClip(kOfxImageEffectOutputClipName));
    assert(outputClip);
    std::map<OfxClipInstance*,OfxImageEffectInstance::ClipPrefs>::iterator foundOutputPrefs = clipPrefs.find(outputClip);
    assert(foundOutputPrefs != clipPrefs.end());

    
    
    std::string outputClipDepth = foundOutputPrefs->second.bitdepth;
    Natron::ImageBitDepthEnum outputClipDepthNatron = OfxClipInstance::ofxDepthToNatronDepth(outputClipDepth);
    
    ///Set a warning on the node if the bitdepth conversion from one of the input clip to the output clip is lossy
    QString bitDepthWarning("This nodes converts higher bit depths images from its inputs to work. As "
                            "a result of this process, the quality of the images is degraded. The following conversions are done: \n");
    bool setBitDepthWarning = false;
    
    bool outputModified = false;
    
    if (!self->isSupportedBitDepth(OfxClipInstance::ofxDepthToNatronDepth(outputClipDepth))) {
        outputClipDepth = self->effectInstance()->bestSupportedDepth(kOfxBitDepthFloat);
        outputClipDepthNatron = OfxClipInstance::ofxDepthToNatronDepth(outputClipDepth);
        foundOutputPrefs->second.bitdepth = outputClipDepth;
        outputModified = true;
    }
    
    double outputAspectRatio = foundOutputPrefs->second.par;
    
    
    ///output clip doesn't support components just remap it, this is probably a plug-in bug.
    if (!outputClip->isSupportedComponent(foundOutputPrefs->second.components)) {
        foundOutputPrefs->second.components = outputClip->findSupportedComp(kOfxImageComponentRGBA);
        outputModified = true;
    }
    
    ///Adjust output premultiplication if needed
    if (foundOutputPrefs->second.components == kOfxImageComponentRGB) {
        effectPrefs.premult = kOfxImageOpaque;
    } else if (foundOutputPrefs->second.components == kOfxImageComponentAlpha) {
        effectPrefs.premult = kOfxImagePreMultiplied;
    } 
    
    
    int maxInputs = self->getMaxInputCount();
    
    for (int i = 0; i < maxInputs; ++i) {
        EffectInstance* inputEffect = self->getInput(i);
        if (inputEffect) {
            inputEffect = inputEffect->getNearestNonIdentity(time);
        }
        OfxEffectInstance* instance = dynamic_cast<OfxEffectInstance*>(inputEffect);
        OfxClipInstance* clip = self->getClipCorrespondingToInput(i);
        
        if (instance) {
            
            std::map<OfxClipInstance*,OfxImageEffectInstance::ClipPrefs>::iterator foundClipPrefs = clipPrefs.find(clip);
            assert(foundClipPrefs != clipPrefs.end());

            bool hasChanged = false;
            
            ///This is the output clip of the input node
            OFX::Host::ImageEffect::ClipInstance* inputOutputClip = instance->effectInstance()->getClip(kOfxImageEffectOutputClipName);
            
            ///Set the clip to have the same components as the output components if it is supported
            if ( clip->isSupportedComponent(foundOutputPrefs->second.components) ) {
                ///we only take into account non mask clips for the most components
                if ( !clip->isMask() && (foundClipPrefs->second.components != foundOutputPrefs->second.components) ) {
                    foundClipPrefs->second.components = foundOutputPrefs->second.components;
                    hasChanged = true;
                }
            }
            
            ///Try to remap the clip's bitdepth to be the same as
            const std::string & input_outputDepth = inputOutputClip->getPixelDepth();
            Natron::ImageBitDepthEnum input_outputNatronDepth = OfxClipInstance::ofxDepthToNatronDepth(input_outputDepth);
            
            ///If supported, set the clip's bitdepth to be the same as the output depth of the input node
            if ( self->isSupportedBitDepth(input_outputNatronDepth) ) {
                bool depthsDifferent = input_outputNatronDepth != outputClipDepthNatron;
                if (self->effectInstance()->supportsMultipleClipDepths() && depthsDifferent) {
                    foundClipPrefs->second.bitdepth = input_outputDepth;
                    hasChanged = true;
                }
            }
            ///Otherwise if the bit-depth conversion will be lossy, warn the user
            else if ( Image::isBitDepthConversionLossy(input_outputNatronDepth, outputClipDepthNatron) ) {
                bitDepthWarning.append( instance->getName().c_str() );
                bitDepthWarning.append(" (" + QString( Image::getDepthString(input_outputNatronDepth).c_str() ) + ")");
                bitDepthWarning.append(" ----> ");
                bitDepthWarning.append( self->getName_mt_safe().c_str() );
                bitDepthWarning.append(" (" + QString( Image::getDepthString(outputClipDepthNatron).c_str() ) + ")");
                bitDepthWarning.append('\n');
                setBitDepthWarning = true;
            }
            
            if (!self->effectInstance()->supportsMultipleClipPARs() && foundClipPrefs->second.par != outputAspectRatio && foundClipPrefs->first->getConnected()) {
                qDebug() << self->getName_mt_safe().c_str() << ": An input clip ("<< foundClipPrefs->first->getName().c_str()
                << ") has a pixel aspect ratio (" << foundClipPrefs->second.par
                << ") different than the output clip (" << outputAspectRatio << ") but it doesn't support multiple clips PAR. "
                << "This should have been handled earlier before connecting the nodes, @see Node::canConnectInput.";
            }
            
            if (hasChanged) {
                changedClips.push_back(clip);
            }
        }
    }
    
    if (outputModified) {
        changedClips.push_back(outputClip);
    }
    
    self->getNode()->toggleBitDepthWarning(setBitDepthWarning, bitDepthWarning);
    
} //endCheckOFXClipPreferences



void
OfxEffectInstance::checkOFXClipPreferences(double time,
                                           const RenderScale & scale,
                                           const std::string & reason,
                                           bool forceGetClipPrefAction)
{
        
    assert(_context != eContextNone);
    assert( QThread::currentThread() == qApp->thread() );
    
    ////////////////////////////////////////////////////////////////
    ///////////////////////////////////
    //////////////// STEP 1 : Get plug-in render preferences
    std::map<OfxClipInstance*,OfxImageEffectInstance::ClipPrefs> clipsPrefs;
    OfxImageEffectInstance::EffectPrefs effectPrefs;
    {
        RECURSIVE_ACTION();
        SET_CAN_SET_VALUE(false);
        
        ///Take the preferences lock so that it cannot be modified throughout the action.
        QWriteLocker preferencesLocker(_preferencesLock);
        if (forceGetClipPrefAction) {
            if (!_effect->getClipPreferences_safe(clipsPrefs,effectPrefs)) {
                return;
            }
        } else {
            if (_effect->areClipPrefsDirty()) {
                if (!_effect->getClipPreferences_safe(clipsPrefs, effectPrefs)) {
                    return;
                }
            } else {
                return;
            }
        }
    }
    
    
    ////////////////////////////////////////////////////////////////
    ////////////////////////////////
    //////////////// STEP 2: Apply a proxy, i.e: modify the preferences so it requires a minimum pixel shuffling
    std::list<OfxClipInstance*> modifiedClips;
    
    clipPrefsProxy(this,time,clipsPrefs,effectPrefs,modifiedClips);
    
    
    ////////////////////////////////////////////////////////////////
    ////////////////////////////////
    //////////////// STEP 3: Actually push to the clips the preferences and set the flags on the effect, protected by a write lock.
    
    {
        QWriteLocker l(_preferencesLock);
        for (std::map<OfxClipInstance*,OfxImageEffectInstance::ClipPrefs>::const_iterator it = clipsPrefs.begin(); it != clipsPrefs.end(); ++it) {
            it->first->setComponents(it->second.components);
            it->first->setPixelDepth(it->second.bitdepth);
            it->first->setAspectRatio(it->second.par);
        }
        
        effectInstance()->updatePreferences_safe(effectPrefs.frameRate, effectPrefs.fielding, effectPrefs.premult,
                                                 effectPrefs.continuous, effectPrefs.frameVarying);
    }
    
    
    ////////////////////////////////////////////////////////////////
    ////////////////////////////////
    //////////////// STEP 4: If our proxy remapping changed some clips preferences, notifying the plug-in of the clips which changed
    if (!getApp()->getProject()->isLoadingProject()) {
        RECURSIVE_ACTION();
        SET_CAN_SET_VALUE(true);
        if (!modifiedClips.empty()) {
            effectInstance()->beginInstanceChangedAction(reason);
        }
        for (std::list<OfxClipInstance*>::iterator it = modifiedClips.begin(); it!=modifiedClips.end();++it) {
            effectInstance()->clipInstanceChangedAction((*it)->getName(), reason, time, scale);
        }
        if (!modifiedClips.empty()) {
            effectInstance()->endInstanceChangedAction(reason);
        }
    }
    
    
} // checkOFXClipPreferences

void
OfxEffectInstance::restoreClipPreferences()
{
    assert(_context != eContextNone);

    double time = getApp()->getTimeLine()->currentFrame();
    RenderScale s;
    s.x = s.y = 1.;
    
    ///if all non optional clips are connected, call getClipPrefs
    ///The clip preferences action is never called until all non optional clips have been attached to the plugin.
    if ( _effect->areAllNonOptionalClipsConnected() ) {
        
        ///Render scale support might not have been set already because getRegionOfDefinition could have failed until all non optional inputs were connected
        if (supportsRenderScaleMaybe() == eSupportsMaybe) {
            OfxRectD rod;
            OfxPointD scaleOne;
            scaleOne.x = scaleOne.y = 1.;
            OfxStatus rodstat = _effect->getRegionOfDefinitionAction(time, scaleOne, rod);
            if ( (rodstat == kOfxStatOK) || (rodstat == kOfxStatReplyDefault) ) {
                OfxPointD scale;
                scale.x = 0.5;
                scale.y = 0.5;
                rodstat = _effect->getRegionOfDefinitionAction(time, scale, rod);
                if ( (rodstat == kOfxStatOK) || (rodstat == kOfxStatReplyDefault) ) {
                    setSupportsRenderScaleMaybe(eSupportsYes);
                } else {
                    setSupportsRenderScaleMaybe(eSupportsNo);
                }
            }
            
        }

        
        checkOFXClipPreferences_public(time,s,kOfxChangeUserEdited,true, false);
    }
}

std::vector<std::string>
OfxEffectInstance::supportedFileFormats() const
{
    assert(_context != eContextNone);
    int formatsCount = _effect->getDescriptor().getProps().getDimension(kTuttleOfxImageEffectPropSupportedExtensions);
    std::vector<std::string> formats(formatsCount);
    for (int k = 0; k < formatsCount; ++k) {
        formats[k] = _effect->getDescriptor().getProps().getStringProperty(kTuttleOfxImageEffectPropSupportedExtensions,k);
        std::transform(formats[k].begin(), formats[k].end(), formats[k].begin(), ::tolower);
    }

    return formats;
}

Natron::StatusEnum
OfxEffectInstance::getRegionOfDefinition(U64 hash,
                                         SequenceTime time,
                                         const RenderScale & scale,
                                         int view,
                                         RectD* rod)
{
    assert(_context != eContextNone);
    if (!_initialized) {
        return Natron::eStatusFailed;
    }

    assert(_effect);

    unsigned int mipMapLevel = Image::getLevelFromScale(scale.x);

    // getRegionOfDefinition may be the first action with renderscale called on any effect.
    // it may have to check for render scale support.
    SupportsEnum supportsRS = supportsRenderScaleMaybe();
    bool scaleIsOne = (scale.x == 1. && scale.y == 1.);
    if ( (supportsRS == eSupportsNo) && !scaleIsOne ) {
        qDebug() << "getRegionOfDefinition called with render scale != 1, but effect does not support render scale!";

        return eStatusFailed;
    }

    OfxRectD ofxRod;
    OfxStatus stat;
    
    {
        bool skipDiscarding = false;
        if (getRecursionLevel() > 1) {
            skipDiscarding = true;
        }
        
        
        ClipsThreadStorageSetter clipSetter(effectInstance(),
                                            skipDiscarding,
                                            true, //< setView ?
                                            view,
                                            true, //< set mipmaplevel?
                                            mipMapLevel);
        
        {
            if (getRecursionLevel() > 1) {
                stat = _effect->getRegionOfDefinitionAction(time, scale, ofxRod);
            } else {
                ///Take the preferences lock so that it cannot be modified throughout the action.
                QReadLocker preferencesLocker(_preferencesLock);
                stat = _effect->getRegionOfDefinitionAction(time, scale, ofxRod);
            }
        }
        if ( !scaleIsOne && (supportsRS == eSupportsMaybe) ) {
            if ( (stat == kOfxStatOK) || (stat == kOfxStatReplyDefault) ) {
                // we got at least one success with RS != 1
                setSupportsRenderScaleMaybe(eSupportsYes);
            } else if (stat == kOfxStatFailed) {
                // maybe the effect does not support renderscale
                // try again with scale one
                OfxPointD scaleOne;
                scaleOne.x = scaleOne.y = 1.;
                
                {
                    SET_CAN_SET_VALUE(false);
                    
                    if (getRecursionLevel() > 1) {
                        stat = _effect->getRegionOfDefinitionAction(time, scaleOne, ofxRod);
                    } else {
                        ///Take the preferences lock so that it cannot be modified throughout the action.
                        QReadLocker preferencesLocker(_preferencesLock);
                        stat = _effect->getRegionOfDefinitionAction(time, scaleOne, ofxRod);
                    }
                }
                
                if ( (stat == kOfxStatOK) || (stat == kOfxStatReplyDefault) ) {
                    // we got success with scale = 1, which means it doesn't support renderscale after all
                    setSupportsRenderScaleMaybe(eSupportsNo);
                } else {
                    // if both actions failed, we can't say anything
                    return eStatusFailed;
                }
                if (stat == kOfxStatReplyDefault) {
                    calcDefaultRegionOfDefinition(hash,time,view,scaleOne, rod);

                    return eStatusReplyDefault;
                }
            }
        }
        if ( (stat != kOfxStatOK) && (stat != kOfxStatReplyDefault) ) {
            return eStatusFailed;
        }

        if (stat == kOfxStatReplyDefault) {
            calcDefaultRegionOfDefinition(hash,time,view, scale, rod);

            return eStatusReplyDefault;
        }
    }


    ///If the rod is 1 pixel, determine if it was because one clip was unconnected or this is really a
    ///1 pixel large image
    if ( (ofxRod.x2 == 1.) && (ofxRod.y2 == 1.) && (ofxRod.x1 == 0.) && (ofxRod.y1 == 0.) ) {
        int maxInputs = getMaxInputCount();
        for (int i = 0; i < maxInputs; ++i) {
            OfxClipInstance* clip = getClipCorrespondingToInput(i);
            if ( clip && !clip->getConnected() && !clip->isOptional() && !clip->isMask() ) {
                ///this is a mandatory source clip and it is not connected, return statfailed
                return eStatusFailed;
            }
        }
    }

    RectD::ofxRectDToRectD(ofxRod, rod);

    return eStatusOK;

    // OFX::Host::ImageEffect::ClipInstance* clip = effectInstance()->getClip(kOfxImageEffectOutputClipName);
    //assert(clip);
    //double pa = clip->getAspectRatio();
} // getRegionOfDefinition

void
OfxEffectInstance::calcDefaultRegionOfDefinition(U64 /*hash*/,
                                                 SequenceTime time,
                                                 int view,
                                                 const RenderScale & scale,
                                                 RectD *rod)
{
    assert(_context != eContextNone);
    if (!_initialized) {
        throw std::runtime_error("OfxEffectInstance not initialized");
    }
    
    bool skipDiscarding = false;
    if (getRecursionLevel() > 1) {
        skipDiscarding = true;
    }
    unsigned int mipMapLevel = Image::getLevelFromScale(scale.x);
    OfxRectD ofxRod;
    
    {
        SET_CAN_SET_VALUE(false);
        
        ///Take the preferences lock so that it cannot be modified throughout the action.
        if (getRecursionLevel() == 0) {
            ClipsThreadStorageSetter clipSetter(effectInstance(),
                                                skipDiscarding,
                                                true, //< setView ?
                                                view,
                                                true, //< set mipmaplevel?
                                                mipMapLevel);
            
            
            // from http://openfx.sourceforge.net/Documentation/1.3/ofxProgrammingReference.html#kOfxImageEffectActionGetRegionOfDefinition
            // generator context - defaults to the project window,
            // filter and paint contexts - defaults to the RoD of the 'Source' input clip at the given time,
            // transition context - defaults to the union of the RoDs of the 'SourceFrom' and 'SourceTo' input clips at the given time,
            // general context - defaults to the union of the RoDs of all the effect non optional input clips at the given time, if none exist, then it is the project window
            // retimer context - defaults to the union of the RoD of the 'Source' input clip at the frame directly preceding the value of the 'SourceTime' double parameter and the frame directly after it
            
            // the following ofxh function does the job
            QReadLocker preferencesLocker(_preferencesLock);
            ofxRod = _effect->calcDefaultRegionOfDefinition(time, (OfxPointD)scale);
        } else {
            ofxRod = _effect->calcDefaultRegionOfDefinition(time, (OfxPointD)scale);
        }
    }
    rod->x1 = ofxRod.x1;
    rod->x2 = ofxRod.x2;
    rod->y1 = ofxRod.y1;
    rod->y2 = ofxRod.y2;
}

static void
rectToOfxRectD(const RectD & b,
               OfxRectD *out)
{
    out->x1 = b.left();
    out->x2 = b.right();
    out->y1 = b.bottom();
    out->y2 = b.top();
}

void
OfxEffectInstance::getRegionsOfInterest(SequenceTime time,
                                        const RenderScale & scale,
                                        const RectD & outputRoD,
                                        const RectD & renderWindow, //!< the region to be rendered in the output image, in Canonical Coordinates
                                        int view,
                                        EffectInstance::RoIMap* ret)
{
    assert(_context != eContextNone);
    std::map<OFX::Host::ImageEffect::ClipInstance*,OfxRectD> inputRois;
    if (!_initialized) {
        return;
    }
    assert(outputRoD.x2 >= outputRoD.x1 && outputRoD.y2 >= outputRoD.y1);
    assert(renderWindow.x2 >= renderWindow.x1 && renderWindow.y2 >= renderWindow.y1);

    {
        bool scaleIsOne = (scale.x == 1. && scale.y == 1.);
        assert( !( (supportsRenderScaleMaybe() == eSupportsNo) && !scaleIsOne ) );
    }

    OfxStatus stat;

    ///before calling getRoIaction set the relevant info on the clips

    unsigned int mipMapLevel = Image::getLevelFromScale(scale.x);
    {
        SET_CAN_SET_VALUE(false);

        bool skipDiscarding = false;
        if (getRecursionLevel() > 1) {
            // getRegionsOfInterest may be called recursively as a result of calling fetchImage() from an action
            skipDiscarding = true;
        }
        ClipsThreadStorageSetter clipSetter(effectInstance(),
                                            skipDiscarding,
                                            true, //< setView ?
                                            view,
                                            true,
                                            mipMapLevel);
        OfxRectD roi;
        rectToOfxRectD(renderWindow, &roi);
        
        ///Take the preferences lock so that it cannot be modified throughout the action.
        QReadLocker preferencesLocker(_preferencesLock);
        stat = _effect->getRegionOfInterestAction( (OfxTime)time, scale,
                                                   roi, inputRois );
    }


    if ( (stat != kOfxStatOK) && (stat != kOfxStatReplyDefault) ) {
        appPTR->writeToOfxLog_mt_safe(QString( getNode()->getName_mt_safe().c_str() ) + "Failed to specify the region of interest from inputs.");
    }
    if (stat != kOfxStatReplyDefault) {
        
        for (std::map<OFX::Host::ImageEffect::ClipInstance*,OfxRectD>::iterator it = inputRois.begin(); it != inputRois.end(); ++it) {
            OfxClipInstance* clip = dynamic_cast<OfxClipInstance*>(it->first);
            assert(clip);
            if (clip) {
                EffectInstance* inputNode = clip->getAssociatedNode();
                RectD inputRoi; // input RoI in canonical coordinates
                inputRoi.x1 = it->second.x1;
                inputRoi.x2 = it->second.x2;
                inputRoi.y1 = it->second.y1;
                inputRoi.y2 = it->second.y2;

                ///The RoI might be infinite if the getRoI action of the plug-in doesn't do anything and the input effect has an
                ///infinite rod.
                ifInfiniteclipRectToProjectDefault(&inputRoi);
                ret->insert( std::make_pair(inputNode,inputRoi) );
            }
        }
        
    } else if (stat == kOfxStatReplyDefault) {
        
        const std::map<std::string,OFX::Host::ImageEffect::ClipInstance*>& clips = effectInstance()->getClips();
        for (std::map<std::string,OFX::Host::ImageEffect::ClipInstance*>::const_iterator it = clips.begin(); it!=clips.end(); ++it) {
            if (!it->second->isOutput()) {
                OfxClipInstance* natronClip = dynamic_cast<OfxClipInstance*>(it->second);
                EffectInstance* inputNode = natronClip ? natronClip->getAssociatedNode() : 0;
                if (inputNode) {
                    ret->insert( std::make_pair(inputNode, renderWindow) );
                }
                assert(natronClip);
            }
        }
    }

} // getRegionsOfInterest

Natron::EffectInstance::FramesNeededMap
OfxEffectInstance::getFramesNeeded(SequenceTime time)
{
    assert(_context != eContextNone);
    EffectInstance::FramesNeededMap ret;
    if (!_initialized) {
        return ret;
    }
    OFX::Host::ImageEffect::RangeMap inputRanges;
    assert(_effect);
    OfxStatus stat;
    {
        SET_CAN_SET_VALUE(false);

        ///Take the preferences lock so that it cannot be modified throughout the action.
        QReadLocker preferencesLocker(_preferencesLock);
        stat = _effect->getFrameNeededAction( (OfxTime)time, inputRanges );
    }
    if ( (stat != kOfxStatOK) && (stat != kOfxStatReplyDefault) ) {
        Natron::errorDialog( getName(), QObject::tr("Failed to specify the frame ranges needed from inputs.").toStdString() );
    } else if (stat == kOfxStatOK) {
        for (OFX::Host::ImageEffect::RangeMap::iterator it = inputRanges.begin(); it != inputRanges.end(); ++it) {
            OfxClipInstance* clip = dynamic_cast<OfxClipInstance*>(it->first);
            assert(clip);
            if (clip) {
                int inputNb = clip->getInputNb();
                if (inputNb != -1) {
                    ret.insert( std::make_pair(inputNb,it->second) );
                }
            }
        }
    } else if (stat == kOfxStatReplyDefault) {
        return Natron::EffectInstance::getFramesNeeded(time);
    }

    return ret;
}

void
OfxEffectInstance::getFrameRange(SequenceTime *first,
                                 SequenceTime *last)
{
    assert(_context != eContextNone);
    if (!_initialized) {
        return;
    }
    OfxRangeD range;
    // getTimeDomain should only be called on the 'general', 'reader' or 'generator' contexts.
    //  see http://openfx.sourceforge.net/Documentation/1.3/ofxProgrammingReference.html#kOfxImageEffectActionGetTimeDomain"
    // Edit: Also add the 'writer' context as we need the getTimeDomain action to be able to find out the frame range to render.
    OfxStatus st = kOfxStatReplyDefault;
    if ( (_context == eContextGeneral) ||
         ( _context == eContextReader) ||
         ( _context == eContextWriter) ||
         ( _context == eContextGenerator) ) {
        
        SET_CAN_SET_VALUE(false);
        ///Take the preferences lock so that it cannot be modified throughout the action.
        QReadLocker preferencesLocker(_preferencesLock);
        st = _effect->getTimeDomainAction(range);
    }
    if (st == kOfxStatOK) {
        *first = (SequenceTime)range.min;
        *last = (SequenceTime)range.max;
    } else if (st == kOfxStatReplyDefault) {
        //The default is...
        int nthClip = _effect->getNClips();
        if (nthClip == 0) {
            //infinite if there are no non optional input clips.
            *first = INT_MIN;
            *last = INT_MAX;
        } else {
            //the union of all the frame ranges of the non optional input clips.
            bool firstValidInput = true;
            *first = INT_MIN;
            *last = INT_MAX;

            int inputsCount = getMaxInputCount();

            ///Uncommented the isOptional() introduces a bugs with Genarts Monster plug-ins when 2 generators
            ///are connected in the pipeline. They must rely on the time domain to maintain an internal state and apparantly
            ///not taking optional inputs into accounts messes it up.
            for (int i = 0; i < inputsCount; ++i) {
                //if (!isInputOptional(i)) {
                EffectInstance* inputEffect = getInput(i);
                if (inputEffect) {
                    SequenceTime f,l;
                    inputEffect->getFrameRange_public(inputEffect->getRenderHash(),&f, &l);
                    if (!firstValidInput) {
                        if ( (f < *first) && (f != INT_MIN) ) {
                            *first = f;
                        }
                        if ( (l > *last) && (l != INT_MAX) ) {
                            *last = l;
                        }
                    } else {
                        firstValidInput = false;
                        *first = f;
                        *last = l;
                    }
                }
                // }
            }
        }
    }
} // getFrameRange

bool
OfxEffectInstance::isIdentity(SequenceTime time,
                              const RenderScale & scale,
                              const RectD & rod,
                              const double par,
                              int view,
                              SequenceTime* inputTime,
                              int* inputNb)
{
    if (!_created) {
        *inputNb = -1;
        *inputTime = 0;
        return false;
    }
    
    assert(_context != eContextNone);
    const std::string field = kOfxImageFieldNone; // TODO: support interlaced data
    std::string inputclip;
    OfxTime inputTimeOfx = time;


    // isIdentity may be the first action with renderscale called on any effect.
    // it may have to check for render scale support.
    SupportsEnum supportsRS = supportsRenderScaleMaybe();
    bool scaleIsOne = (scale.x == 1. && scale.y == 1.);
    if ( (supportsRS == eSupportsNo) && !scaleIsOne ) {
        qDebug() << "isIdentity called with render scale != 1, but effect does not support render scale!";
        assert(false);
        throw std::logic_error("isIdentity called with render scale != 1, but effect does not support render scale!");
    }
    
    unsigned int mipMapLevel = Image::getLevelFromScale(scale.x);
    OfxStatus stat;
    
    {
        bool skipDiscarding = false;
        
        if (getRecursionLevel() > 1) {
            
//#ifdef DEBUG
//            if (QThread::currentThread() != qApp->thread()) {
//                qDebug() << "isIdentity cannot be called recursively as an action. Please check this.";
//            }
//#endif
            skipDiscarding = true;
        }
        
        SET_CAN_SET_VALUE(false);

        
        ClipsThreadStorageSetter clipSetter(effectInstance(),
                                            skipDiscarding,
                                            true, //< setView ?
                                            view,
                                            true,
                                            mipMapLevel);
        
        // In Natron, we only consider isIdentity for whole images
        RectI roi;
        rod.toPixelEnclosing(scale, par, &roi);
        OfxRectI ofxRoI;
        ofxRoI.x1 = roi.left();
        ofxRoI.x2 = roi.right();
        ofxRoI.y1 = roi.bottom();
        ofxRoI.y2 = roi.top();
        
        {
            if (getRecursionLevel() > 1) {
                stat = _effect->isIdentityAction(inputTimeOfx, field, ofxRoI, scale, inputclip);
            } else {
                ///Take the preferences lock so that it cannot be modified throughout the action.
                QReadLocker preferencesLocker(_preferencesLock);
                stat = _effect->isIdentityAction(inputTimeOfx, field, ofxRoI, scale, inputclip);
            }
        }
        if ( !scaleIsOne && (supportsRS == eSupportsMaybe) ) {
            if ( (stat == kOfxStatOK) || (stat == kOfxStatReplyDefault) ) {
                // we got at least one success with RS != 1
                setSupportsRenderScaleMaybe(eSupportsYes);
            } else if (stat == kOfxStatFailed) {
                // maybe the effect does not support renderscale
                // try again with scale one
                OfxPointD scaleOne;
                scaleOne.x = scaleOne.y = 1.;
                
                rod.toPixelEnclosing(scaleOne, par, &roi);
                ofxRoI.x1 = roi.left();
                ofxRoI.x2 = roi.right();
                ofxRoI.y1 = roi.bottom();
                ofxRoI.y2 = roi.top();
                
                if (getRecursionLevel() > 1) {
                    stat = _effect->isIdentityAction(inputTimeOfx, field, ofxRoI, scaleOne, inputclip);
                } else {
                    ///Take the preferences lock so that it cannot be modified throughout the action.
                    QReadLocker preferencesLocker(_preferencesLock);
                    stat = _effect->isIdentityAction(inputTimeOfx, field, ofxRoI, scaleOne, inputclip);
                }
                if ( (stat == kOfxStatOK) || (stat == kOfxStatReplyDefault) ) {
                    // we got success with scale = 1, which means it doesn't support renderscale after all
                    setSupportsRenderScaleMaybe(eSupportsNo);
                }
            }
        }
    }

    if (stat == kOfxStatOK) {
        OFX::Host::ImageEffect::ClipInstance* clip = _effect->getClip(inputclip);
        if (!clip) {
            // this is a plugin-side error, don't crash
            qDebug() << "Error in OfxEffectInstance::render(): kOfxImageEffectActionIsIdentity returned an unknown clip: " << inputclip.c_str();

            return false;
        }
        OfxClipInstance* natronClip = dynamic_cast<OfxClipInstance*>(clip);
        assert(natronClip);
        if (!natronClip) {
            // coverity[dead_error_line]
            qDebug() << "Error in OfxEffectInstance::render(): kOfxImageEffectActionIsIdentity returned an unknown clip: " << inputclip.c_str();

            return false;
        }
        *inputTime = inputTimeOfx;

        if ( natronClip->isOutput() ) {
            *inputNb = -2;
        } else {
            *inputNb = natronClip->getInputNb();
        }

        return true;
    } else if (stat == kOfxStatReplyDefault) {
        return false;
    }
    return false; //< may fail if getRegionOfDefinition has failed in the plug-in code
    //throw std::runtime_error("isIdentity failed");
} // isIdentity

Natron::StatusEnum
OfxEffectInstance::beginSequenceRender(SequenceTime first,
                                       SequenceTime last,
                                       SequenceTime step,
                                       bool interactive,
                                       const RenderScale & scale,
                                       bool isSequentialRender,
                                       bool isRenderResponseToUserInteraction,
                                       int view)
{
    {
        bool scaleIsOne = (scale.x == 1. && scale.y == 1.);
        assert( !( (supportsRenderScaleMaybe() == eSupportsNo) && !scaleIsOne ) );
    }

    OfxStatus stat;
    unsigned int mipMapLevel = Image::getLevelFromScale(scale.x);
    {
        bool skipDiscarding = false;
        if (getRecursionLevel() > 1) {
            qDebug() << "beginRenderAction cannot be called recursively as an action. Please check this.";
            skipDiscarding = true;
        }
        ClipsThreadStorageSetter clipSetter(effectInstance(),
                                            skipDiscarding,
                                            true, //< setView ?
                                            view,
                                            true,
                                            mipMapLevel);

        SET_CAN_SET_VALUE(false);

        
        ///Take the preferences lock so that it cannot be modified throughout the action.
        QReadLocker preferencesLocker(_preferencesLock);
        stat = effectInstance()->beginRenderAction(first, last, step, interactive, scale, isSequentialRender, isRenderResponseToUserInteraction, view);
    }

    if ( (stat != kOfxStatOK) && (stat != kOfxStatReplyDefault) ) {
        return eStatusFailed;
    }

    return eStatusOK;
}

Natron::StatusEnum
OfxEffectInstance::endSequenceRender(SequenceTime first,
                                     SequenceTime last,
                                     SequenceTime step,
                                     bool interactive,
                                     const RenderScale & scale,
                                     bool isSequentialRender,
                                     bool isRenderResponseToUserInteraction,
                                     int view)
{
    {
        bool scaleIsOne = (scale.x == 1. && scale.y == 1.);
        assert( !( (supportsRenderScaleMaybe() == eSupportsNo) && !scaleIsOne ) );
    }

    OfxStatus stat;
    unsigned int mipMapLevel = Image::getLevelFromScale(scale.x);
    {
        bool skipDiscarding = false;
        if (getRecursionLevel() > 1) {
            qDebug() << "endRenderAction cannot be called recursively as an action. Please check this.";
            skipDiscarding = true;
        }
        ClipsThreadStorageSetter clipSetter(effectInstance(),
                                            skipDiscarding,
                                            true, //< setView ?
                                            view,
                                            true,
                                            mipMapLevel);
        SET_CAN_SET_VALUE(false);

        
        ///Take the preferences lock so that it cannot be modified throughout the action.
        QReadLocker preferencesLocker(_preferencesLock);
        stat = effectInstance()->endRenderAction(first, last, step, interactive,scale, isSequentialRender, isRenderResponseToUserInteraction, view);
    }

    if ( (stat != kOfxStatOK) && (stat != kOfxStatReplyDefault) ) {
        return eStatusFailed;
    }

    return eStatusOK;
}

Natron::StatusEnum
OfxEffectInstance::render(SequenceTime time,
                          const RenderScale& originalScale,
                          const RenderScale & mappedScale,
                          const RectI & roi,
                          int view,
                          bool isSequentialRender,
                          bool isRenderResponseToUserInteraction,
                          boost::shared_ptr<Natron::Image> output)
{
    if (!_initialized) {
        return Natron::eStatusFailed;
    }

    OfxRectI ofxRoI;
    ofxRoI.x1 = roi.left();
    ofxRoI.x2 = roi.right();
    ofxRoI.y1 = roi.bottom();
    ofxRoI.y2 = roi.top();
    int viewsCount = getApp()->getProject()->getProjectViewsCount();
    OfxStatus stat;
    const std::string field = kOfxImageFieldNone; // TODO: support interlaced data
    ///before calling render, set the render scale thread storage for each clip
# ifdef DEBUG
    {
        // check the dimensions of output images
        const RectI & dstBounds = output->getBounds();
        const RectD & dstRodCanonical = output->getRoD();
        RectI dstRod;
        dstRodCanonical.toPixelEnclosing(mappedScale, output->getPixelAspectRatio(), &dstRod);

        if ( !supportsTiles() ) {
            // http://openfx.sourceforge.net/Documentation/1.3/ofxProgrammingReference.html#kOfxImageEffectPropSupportsTiles
            //  If a clip or plugin does not support tiled images, then the host should supply full RoD images to the effect whenever it fetches one.
            assert(dstRod.x1 == dstBounds.x1);
            assert(dstRod.x2 == dstBounds.x2);
            assert(dstRod.y1 == dstBounds.y1);
            assert(dstRod.y2 == dstBounds.y2);
        }
        if ( !supportsMultiResolution() ) {
            // http://openfx.sourceforge.net/Documentation/1.3/ofxProgrammingReference.html#kOfxImageEffectPropSupportsMultiResolution
            //   Multiple resolution images mean...
            //    input and output images can be of any size
            //    input and output images can be offset from the origin
            assert(dstRod.x1 == 0);
            assert(dstRod.y1 == 0);
        }
    }
# endif // DEBUG
    {
        bool skipDiscarding = false;
        if (getRecursionLevel() > 1) {
            qDebug() << "renderAction cannot be called recursively as an action. Please check this.";
            skipDiscarding = true;
        }
        
        SET_CAN_SET_VALUE(false);

        
        ClipsThreadStorageSetter clipSetter(effectInstance(),
                                            skipDiscarding,
                                            true, //< setView ?
                                            view,
                                            true,//< set mipmaplevel ?
                                            Natron::Image::getLevelFromScale(originalScale.x));

        
        ///Take the preferences lock so that it cannot be modified throughout the action.
        QReadLocker preferencesLocker(_preferencesLock);
        stat = _effect->renderAction( (OfxTime)time,
                                      field,
                                      ofxRoI,
                                      mappedScale,
                                      isSequentialRender,
                                      isRenderResponseToUserInteraction,
                                      view,
                                      viewsCount );
    }

    if (stat != kOfxStatOK) {
        return eStatusFailed;
    } else {
        return eStatusOK;
    }
} // render

bool
OfxEffectInstance::supportsMultipleClipsPAR() const
{
    return _effect->supportsMultipleClipPARs();
}

EffectInstance::RenderSafetyEnum
OfxEffectInstance::renderThreadSafety() const
{
    {
        QReadLocker readL(_renderSafetyLock);
        if (_wasRenderSafetySet) {
            return _renderSafety;
        }
    }
    {
        QWriteLocker writeL(_renderSafetyLock);
        const std::string & safety = _effect->getRenderThreadSafety();
        if (safety == kOfxImageEffectRenderUnsafe) {
            _renderSafety =  EffectInstance::eRenderSafetyUnsafe;
        } else if (safety == kOfxImageEffectRenderInstanceSafe) {
            _renderSafety = EffectInstance::eRenderSafetyInstanceSafe;
        } else if (safety == kOfxImageEffectRenderFullySafe) {
            if ( _effect->getHostFrameThreading() ) {
                _renderSafety =  EffectInstance::eRenderSafetyFullySafeFrame;
            } else {
                _renderSafety =  EffectInstance::eRenderSafetyFullySafe;
            }
        } else {
            qDebug() << "Unknown thread safety level: " << safety.c_str();
            _renderSafety =  EffectInstance::eRenderSafetyUnsafe;
        }
        _wasRenderSafetySet = true;

        return _renderSafety;
    }
}

bool
OfxEffectInstance::makePreviewByDefault() const
{
    return isGenerator();
}

const std::string &
OfxEffectInstance::getShortLabel() const
{
    return effectInstance()->getShortLabel();
}

void
OfxEffectInstance::initializeOverlayInteract()
{
    tryInitializeOverlayInteracts();
}

void
OfxEffectInstance::drawOverlay(double scaleX,
                               double scaleY)
{
    if (!_initialized) {
        return;
    }
    if (_overlayInteract) {
        OfxPointD rs;
        rs.x = scaleX;
        rs.y = scaleY;
        OfxTime time = getApp()->getTimeLine()->currentFrame();

       /* if (getRecursionLevel() == 1) {
            
            
            bool skipDiscarding = false;
            if (getRecursionLevel() > 1) {
                ///This happens sometimes because of dialogs popping from the request of a plug-in (inside an action)
                ///and making the mainwindow loose focus, hence forcing a new paint event
                //qDebug() << "drawAction cannot be called recursively as an action. Please check this.";
                skipDiscarding = true;
            }
            ClipsThreadStorageSetter clipSetter(effectInstance(),
                                                skipDiscarding,
                                                false, //< setView ?
                                                0,
                                                false,
                                                0);
             
            _overlayInteract->drawAction(time, rs);
        } else {*/
        
        SET_CAN_SET_VALUE(false);

        _overlayInteract->drawAction(time, rs);
        
        /*}*/
    }
}

void
OfxEffectInstance::setCurrentViewportForOverlays(OverlaySupport* viewport)
{
    if (_overlayInteract) {
        _overlayInteract->setCallingViewport(viewport);
    }
}

bool
OfxEffectInstance::onOverlayPenDown(double scaleX,
                                    double scaleY,
                                    const QPointF & viewportPos,
                                    const QPointF & pos)
{
    if (!_initialized) {
        return false;
    }
    if (_overlayInteract) {
        OfxPointD rs;
        rs.x = scaleX;
        rs.y = scaleY;
        OfxPointD penPos;
        penPos.x = pos.x();
        penPos.y = pos.y();
        OfxPointI penPosViewport;
        penPosViewport.x = viewportPos.x();
        penPosViewport.y = viewportPos.y();

        OfxTime time = getApp()->getTimeLine()->currentFrame();
        /*
        ClipsThreadStorageSetter clipSetter(effectInstance(),
                                            false,
                                            true, //< setView ?
                                            view,
                                            false,
                                            0);
         */
        SET_CAN_SET_VALUE(true);

        OfxStatus stat = _overlayInteract->penDownAction(time, rs, penPos, penPosViewport, 1.);
        
        if (getRecursionLevel() == 1 && checkIfOverlayRedrawNeeded()) {
            stat = _overlayInteract->redraw();
            assert(stat == kOfxStatOK || stat == kOfxStatReplyDefault);
        }

        if (stat == kOfxStatOK) {
            _penDown = true;

            return true;
        }
    }

    return false;
}

bool
OfxEffectInstance::onOverlayPenMotion(double scaleX,
                                      double scaleY,
                                      const QPointF & viewportPos,
                                      const QPointF & pos)
{
    if (!_initialized) {
        return false;
    }
    if (_overlayInteract) {
        OfxPointD rs;
        rs.x = scaleX;
        rs.y = scaleY;
        OfxPointD penPos;
        penPos.x = pos.x();
        penPos.y = pos.y();
        OfxPointI penPosViewport;
        penPosViewport.x = viewportPos.x();
        penPosViewport.y = viewportPos.y();
        OfxTime time = getApp()->getTimeLine()->currentFrame();
        OfxStatus stat;
        /*
        if (getRecursionLevel() == 1) {
            ClipsThreadStorageSetter clipSetter(effectInstance(),
                                                false,
                                                true, //< setView ?
                                                view,
                                                false,
                                                0);
            stat = _overlayInteract->penMotionAction(time, rs, penPos, penPosViewport, 1.);
        } else {*/
        SET_CAN_SET_VALUE(true);
        stat = _overlayInteract->penMotionAction(time, rs, penPos, penPosViewport, 1.);
        /*}*/
        
        if (getRecursionLevel() == 1 && checkIfOverlayRedrawNeeded()) {
            stat = _overlayInteract->redraw();
            assert(stat == kOfxStatOK || stat == kOfxStatReplyDefault);
        }

        if (stat == kOfxStatOK) {
            return true;
        }
    }

    return false;
}

bool
OfxEffectInstance::onOverlayPenUp(double scaleX,
                                  double scaleY,
                                  const QPointF & viewportPos,
                                  const QPointF & pos)
{
    if (!_initialized) {
        return false;
    }
    if (_overlayInteract) {
        OfxPointD rs;
        rs.x = scaleX;
        rs.y = scaleY;
        OfxPointD penPos;
        penPos.x = pos.x();
        penPos.y = pos.y();
        OfxPointI penPosViewport;
        penPosViewport.x = viewportPos.x();
        penPosViewport.y = viewportPos.y();
        OfxTime time = getApp()->getTimeLine()->currentFrame();
        
        /*
        ClipsThreadStorageSetter clipSetter(effectInstance(),
                                            false,
                                            true,
                                            view,
                                            false,
                                            0);
         */
        SET_CAN_SET_VALUE(true);
        OfxStatus stat = _overlayInteract->penUpAction(time, rs, penPos, penPosViewport, 1.);

        if (getRecursionLevel() == 1 && checkIfOverlayRedrawNeeded()) {
            stat = _overlayInteract->redraw();
            assert(stat == kOfxStatOK || stat == kOfxStatReplyDefault);
        }
        
        if (stat == kOfxStatOK) {
            _penDown = false;

            return true;
        }
    }

    return false;
}

bool
OfxEffectInstance::onOverlayKeyDown(double scaleX,
                                    double scaleY,
                                    Natron::Key key,
                                    Natron::KeyboardModifiers /*modifiers*/)
{
    if (!_initialized) {
        return false;;
    }
    if (_overlayInteract) {
        OfxPointD rs;
        rs.x = scaleX;
        rs.y = scaleY;
        OfxTime time = getApp()->getTimeLine()->currentFrame();
        QByteArray keyStr;
/*
        ClipsThreadStorageSetter clipSetter(effectInstance(),
                                            false,
                                            true, //< setView ?
                                            view,
                                            false,
                                            0);
 */
        SET_CAN_SET_VALUE(true);
        OfxStatus stat = _overlayInteract->keyDownAction( time, rs, (int)key, keyStr.data() );

        if (getRecursionLevel() == 1 && checkIfOverlayRedrawNeeded()) {
            stat = _overlayInteract->redraw();
            assert(stat == kOfxStatOK || stat == kOfxStatReplyDefault);
        }
        
        if (stat == kOfxStatOK) {
            return true;
        }
    }

    return false;
}

bool
OfxEffectInstance::onOverlayKeyUp(double scaleX,
                                  double scaleY,
                                  Natron::Key key,
                                  Natron::KeyboardModifiers /* modifiers*/)
{
    if (!_initialized) {
        return false;
    }
    if (_overlayInteract) {
        OfxPointD rs;
        rs.x = scaleX;
        rs.y = scaleY;
        OfxTime time = getApp()->getTimeLine()->currentFrame();
        QByteArray keyStr;

        /*
        ClipsThreadStorageSetter clipSetter(effectInstance(),
                                            false,
                                            true, //< setView ?
                                            view,
                                            false,
                                            0);
         */
        SET_CAN_SET_VALUE(true);
        OfxStatus stat = _overlayInteract->keyUpAction( time, rs, (int)key, keyStr.data() );

        if (getRecursionLevel() == 1 && checkIfOverlayRedrawNeeded()) {
            stat = _overlayInteract->redraw();
            assert(stat == kOfxStatOK || stat == kOfxStatReplyDefault);
        }
        
        //assert(stat == kOfxStatOK || stat == kOfxStatReplyDefault);
        if (stat == kOfxStatOK) {
            return true;
        }
        ;
    }

    return false;
}

bool
OfxEffectInstance::onOverlayKeyRepeat(double scaleX,
                                      double scaleY,
                                      Natron::Key key,
                                      Natron::KeyboardModifiers /*modifiers*/)
{
    if (!_initialized) {
        return false;
    }
    if (_overlayInteract) {
        OfxPointD rs;
        rs.x = scaleX;
        rs.y = scaleY;
        OfxTime time = getApp()->getTimeLine()->currentFrame();
        QByteArray keyStr;
/*
        ClipsThreadStorageSetter clipSetter(effectInstance(),
                                            false,
                                            true, //< setView ?
                                            view,
                                            false,
                                            0);
 */
        SET_CAN_SET_VALUE(true);
        OfxStatus stat = _overlayInteract->keyRepeatAction( time, rs, (int)key, keyStr.data() );

        if (getRecursionLevel() == 1 && checkIfOverlayRedrawNeeded()) {
            stat = _overlayInteract->redraw();
            assert(stat == kOfxStatOK || stat == kOfxStatReplyDefault);
        }
        
        if (stat == kOfxStatOK) {
            return true;
        }
    }

    return false;
}

bool
OfxEffectInstance::onOverlayFocusGained(double scaleX,
                                        double scaleY)
{
    if (!_initialized) {
        return false;
    }
    if (_overlayInteract) {
        OfxPointD rs;
        rs.x = scaleX;
        rs.y = scaleY;
        OfxTime time = getApp()->getTimeLine()->currentFrame();
        OfxStatus stat;
/*
        if (getRecursionLevel() == 1) {
            ClipsThreadStorageSetter clipSetter(effectInstance(),
                                                false,
                                                true, //< setView ?
                                                view,
                                                false,
                                                0);
            stat = _overlayInteract->gainFocusAction(time, rs);
        } else {*/
        SET_CAN_SET_VALUE(true);
        stat = _overlayInteract->gainFocusAction(time, rs);
        /*}*/
        //assert(stat == kOfxStatOK || stat == kOfxStatReplyDefault);
        if (stat == kOfxStatOK) {
            return true;
        }
    }

    return false;
}

bool
OfxEffectInstance::onOverlayFocusLost(double scaleX,
                                      double scaleY)
{
    if (!_initialized) {
        return false;
    }
    if (_overlayInteract) {
        OfxPointD rs;
        rs.x = scaleX;
        rs.y = scaleY;
        OfxTime time = getApp()->getTimeLine()->currentFrame();
        OfxStatus stat;
        
        /*if (getRecursionLevel() == 1) {
            ClipsThreadStorageSetter clipSetter(effectInstance(),
                                                false,
                                                true, //< setView ?
                                                view,
                                                false,
                                                0);


            stat = _overlayInteract->loseFocusAction(time, rs);
        } else {*/
        SET_CAN_SET_VALUE(true);
        stat = _overlayInteract->loseFocusAction(time, rs);
        /*}*/

        //assert(stat == kOfxStatOK || stat == kOfxStatReplyDefault);
        if (stat == kOfxStatOK) {
            return true;
        }
    }

    return false;
}

bool
OfxEffectInstance::hasOverlay() const
{
    return _overlayInteract != NULL;
}

static std::string
natronValueChangedReasonToOfxValueChangedReason(Natron::ValueChangedReasonEnum reason)
{
    switch (reason) {
        case Natron::eValueChangedReasonUserEdited:
        case Natron::eValueChangedReasonNatronGuiEdited:
            return kOfxChangeUserEdited;
        case Natron::eValueChangedReasonPluginEdited:
        case Natron::eValueChangedReasonNatronInternalEdited:
        case Natron::eValueChangedReasonSlaveRefresh:
        case Natron::eValueChangedReasonRestoreDefault:
            return kOfxChangePluginEdited;
        case Natron::eValueChangedReasonTimeChanged:
            return kOfxChangeTime;
        default:
            assert(false);     // all Natron reasons should be processed
            return "";
    }
}

void
OfxEffectInstance::knobChanged(KnobI* k,
                               Natron::ValueChangedReasonEnum reason,
                               int view,
                               SequenceTime time,
                               bool originatedFromMainThread)
{
    if (!_initialized) {
        return;
    }

    ///If the param changed is a button and the node is disabled don't do anything which might
    ///trigger an analysis
    if ( (reason == eValueChangedReasonUserEdited) && dynamic_cast<Button_Knob*>(k) && _node->isNodeDisabled() ) {
        return;
    }

    if ( _renderButton && ( k == _renderButton.get() ) ) {
        ///don't do anything since it is handled upstream
        return;
    }


    // OFX::Host::Param::paramSetValue() does it for us when it's edited by the plugin
    bool canCallInstanceChangedAction = reason != Natron::eValueChangedReasonPluginEdited;
    
    std::string ofxReason = natronValueChangedReasonToOfxValueChangedReason(reason);
    assert( !ofxReason.empty() ); // crashes when resetting to defaults
    OfxPointD renderScale;
    if (isDoingInteractAction()) {
        assert(_overlayInteract);
        OverlaySupport* lastInteract = _overlayInteract->getLastCallingViewport();
        assert(lastInteract);
        unsigned int mmLevel = lastInteract->getCurrentRenderScale();
        renderScale.x = renderScale.y = 1 << mmLevel;
    } else {
        renderScale.x = renderScale.y = 1;
    }
    OfxStatus stat = kOfxStatOK;
    
    int recursionLevel = getRecursionLevel();

    if (canCallInstanceChangedAction) {
        if (recursionLevel == 1) {
            SET_CAN_SET_VALUE(true);
            ClipsThreadStorageSetter clipSetter(effectInstance(),
                                                false,
                                                true, //< setView ?
                                                view,
                                                true, //< setmipmaplevel?
                                                0);
            
            ///This action as all the overlay interacts actions can trigger recursive actions, such as
            ///getClipPreferences() so we don't take the clips preferences lock for read here otherwise we would
            ///create a deadlock. This code then assumes that the instance changed action of the plug-in doesn't require
            ///the clip preferences to stay the same throughout the action.
            stat = effectInstance()->paramInstanceChangedAction(k->getName(), ofxReason,(OfxTime)time,renderScale);
        } else {
            ///This action as all the overlay interacts actions can trigger recursive actions, such as
            ///getClipPreferences() so we don't take the clips preferences lock for read here otherwise we would
            ///create a deadlock. This code then assumes that the instance changed action of the plug-in doesn't require
            ///the clip preferences to stay the same throughout the action.
            stat = effectInstance()->paramInstanceChangedAction(k->getName(), ofxReason,(OfxTime)time,renderScale);
        }
    }
    if ( (stat != kOfxStatOK) && (stat != kOfxStatReplyDefault) ) {
        QString err( QString( getNode()->getName_mt_safe().c_str() ) + ": An error occured while changing parameter " +
                    k->getDescription().c_str() );
        appPTR->writeToOfxLog_mt_safe(err);
        
        return;
    }
    
    if (QThread::currentThread() == qApp->thread() &&
        originatedFromMainThread) { //< change didnt occur in main-thread in the first, palce don't attempt to draw the overlay
        
        ///Run the following only in the main-thread
        
        if ( _effect->isClipPreferencesSlaveParam( k->getName() ) ) {
            RECURSIVE_ACTION();
            checkOFXClipPreferences_public(time, renderScale, ofxReason,true, true);
        }
#pragma message WARN("TODO (python): also check if the interact is actually visible (requires changes in GUI)")
        if (_overlayInteract) {
            // Some plugins (e.g. by digital film tools) forget to set kOfxInteractPropSlaveToParam.
            // Most hosts trigger a redraw if the plugin has an active overlay.
            //if (std::find(_overlaySlaves.begin(), _overlaySlaves.end(), (void*)k) != _overlaySlaves.end()) {
            incrementRedrawNeededCounter();
            //}

            if (recursionLevel == 1 && checkIfOverlayRedrawNeeded()) {
                stat = _overlayInteract->redraw();
                assert(stat == kOfxStatOK || stat == kOfxStatReplyDefault);
            }
        }
    }
} // knobChanged

void
OfxEffectInstance::beginKnobsValuesChanged(Natron::ValueChangedReasonEnum reason)
{
    if (!_initialized) {
        return;
    }
    
    RECURSIVE_ACTION();
    SET_CAN_SET_VALUE(true);
    ///This action as all the overlay interacts actions can trigger recursive actions, such as
    ///getClipPreferences() so we don't take the clips preferences lock for read here otherwise we would
    ///create a deadlock. This code then assumes that the instance changed action of the plug-in doesn't require
    ///the clip preferences to stay the same throughout the action.
    ignore_result(effectInstance()->beginInstanceChangedAction(natronValueChangedReasonToOfxValueChangedReason(reason)));
}

void
OfxEffectInstance::endKnobsValuesChanged(Natron::ValueChangedReasonEnum reason)
{
    if (!_initialized) {
        return;
    }
    
    RECURSIVE_ACTION();
    SET_CAN_SET_VALUE(true);
    ///This action as all the overlay interacts actions can trigger recursive actions, such as
    ///getClipPreferences() so we don't take the clips preferences lock for read here otherwise we would
    ///create a deadlock. This code then assumes that the instance changed action of the plug-in doesn't require
    ///the clip preferences to stay the same throughout the action.
    ignore_result(effectInstance()->endInstanceChangedAction(natronValueChangedReasonToOfxValueChangedReason(reason)));

}

void
OfxEffectInstance::purgeCaches()
{
    // The kOfxActionPurgeCaches is an action that may be passed to a plug-in instance from time to time in low memory situations. Instances recieving this action should destroy any data structures they may have and release the associated memory, they can later reconstruct this from the effect's parameter set and associated information. http://openfx.sourceforge.net/Documentation/1.3/ofxProgrammingReference.html#kOfxActionPurgeCaches
    OfxStatus stat;
    {
        SET_CAN_SET_VALUE(false);
        ///Take the preferences lock so that it cannot be modified throughout the action.
        QReadLocker preferencesLocker(_preferencesLock);
        stat =  _effect->purgeCachesAction();
        
        assert(stat == kOfxStatOK || stat == kOfxStatReplyDefault);
        
    }
    // The kOfxActionSyncPrivateData action is called when a plugin should synchronise any private data structures to its parameter set. This generally occurs when an effect is about to be saved or copied, but it could occur in other situations as well. http://openfx.sourceforge.net/Documentation/1.3/ofxProgrammingReference.html#kOfxActionSyncPrivateData
    
    {
        RECURSIVE_ACTION();
        SET_CAN_SET_VALUE(true);
        ///This action as all the overlay interacts actions can trigger recursive actions, such as
        ///getClipPreferences() so we don't take the clips preferences lock for read here otherwise we would
        ///create a deadlock. This code then assumes that the instance changed action of the plug-in doesn't require
        ///the clip preferences to stay the same throughout the action.
        stat =  _effect->syncPrivateDataAction();
        assert(stat == kOfxStatOK || stat == kOfxStatReplyDefault);
        
    }
}

int
OfxEffectInstance::getMajorVersion() const
{
    return effectInstance()->getPlugin()->getVersionMajor();
}

int
OfxEffectInstance::getMinorVersion() const
{
    return effectInstance()->getPlugin()->getVersionMinor();
}

bool
OfxEffectInstance::supportsTiles() const
{
    OFX::Host::ImageEffect::ClipInstance* outputClip =  effectInstance()->getClip(kOfxImageEffectOutputClipName);

    if (!outputClip) {
        return false;
    }

    return effectInstance()->supportsTiles() && outputClip->supportsTiles();
}

bool
OfxEffectInstance::supportsMultiResolution() const
{
    return effectInstance()->supportsMultiResolution();
}

void
OfxEffectInstance::beginEditKnobs()
{
    ///Take the preferences lock so that it cannot be modified throughout the action.
    QReadLocker preferencesLocker(_preferencesLock);
    effectInstance()->beginInstanceEditAction();
}

void
OfxEffectInstance::onSyncPrivateDataRequested()
{
    ///Can only be called in the main thread
    assert( QThread::currentThread() == qApp->thread() );
    
    RECURSIVE_ACTION();
    
    ///This action as all the overlay interacts actions can trigger recursive actions, such as
    ///getClipPreferences() so we don't take the clips preferences lock for read here otherwise we would
    ///create a deadlock. This code then assumes that the instance changed action of the plug-in doesn't require
    ///the clip preferences to stay the same throughout the action.
    SET_CAN_SET_VALUE(true);
    effectInstance()->syncPrivateDataAction();
}

void
OfxEffectInstance::addAcceptedComponents(int inputNb,
                                         std::list<Natron::ImageComponentsEnum>* comps)
{
    if (inputNb >= 0) {
        OfxClipInstance* clip = getClipCorrespondingToInput(inputNb);
        assert(clip);
        const std::vector<std::string> & supportedComps = clip->getSupportedComponents();
        for (U32 i = 0; i < supportedComps.size(); ++i) {
            try {
                comps->push_back( OfxClipInstance::ofxComponentsToNatronComponents(supportedComps[i]) );
            } catch (const std::runtime_error &e) {
                // ignore unsupported components
            }
        }
    } else {
        assert(inputNb == -1);
        OfxClipInstance* clip = dynamic_cast<OfxClipInstance*>( effectInstance()->getClip(kOfxImageEffectOutputClipName) );
        assert(clip);
        const std::vector<std::string> & supportedComps = clip->getSupportedComponents();
        for (U32 i = 0; i < supportedComps.size(); ++i) {
            try {
                comps->push_back( OfxClipInstance::ofxComponentsToNatronComponents(supportedComps[i]) );
            } catch (const std::runtime_error &e) {
                // ignore unsupported components
            }
        }
    }
}

void
OfxEffectInstance::addSupportedBitDepth(std::list<Natron::ImageBitDepthEnum>* depths) const
{
    const OFX::Host::Property::Set & prop = effectInstance()->getPlugin()->getDescriptor().getParamSetProps();
    int dim = prop.getDimension(kOfxImageEffectPropSupportedPixelDepths);

    for (int i = 0; i < dim; ++i) {
        const std::string & depth = prop.getStringProperty(kOfxImageEffectPropSupportedPixelDepths,i);
        try {
            depths->push_back( OfxClipInstance::ofxDepthToNatronDepth(depth) );
        } catch (const std::runtime_error &e) {
            // ignore unsupported bitdepth
        }
    }
}


void
OfxEffectInstance::getPreferredDepthAndComponents(int inputNb,
                                                  Natron::ImageComponentsEnum* comp,
                                                  Natron::ImageBitDepthEnum* depth) const
{
    OfxClipInstance* clip;

    if (inputNb == -1) {
        clip = dynamic_cast<OfxClipInstance*>( _effect->getClip(kOfxImageEffectOutputClipName) );
    } else {
        clip = getClipCorrespondingToInput(inputNb);
    }
    assert(clip);
    
    if (getRecursionLevel() > 0) {
        ///Someone took the read  (all actions) or write (getClipPreferences action)lock already
        *comp = OfxClipInstance::ofxComponentsToNatronComponents( clip->getComponents() );
        *depth = OfxClipInstance::ofxDepthToNatronDepth( clip->getPixelDepth() );
    } else {
        ///Take the preferences lock to be sure we're not writing them
        QReadLocker l(_preferencesLock);
        *comp = OfxClipInstance::ofxComponentsToNatronComponents( clip->getComponents() );
        *depth = OfxClipInstance::ofxDepthToNatronDepth( clip->getPixelDepth() );
    }
}

Natron::SequentialPreferenceEnum
OfxEffectInstance::getSequentialPreference() const
{
    int sequential = _effect->getPlugin()->getDescriptor().getProps().getIntProperty(kOfxImageEffectInstancePropSequentialRender);

    switch (sequential) {
    case 0:

        return Natron::eSequentialPreferenceNotSequential;
    case 1:

        return Natron::eSequentialPreferenceOnlySequential;
    case 2:

        return Natron::eSequentialPreferencePreferSequential;
    default:

        return Natron::eSequentialPreferenceNotSequential;
        break;
    }
}

Natron::ImagePremultiplicationEnum
OfxEffectInstance::getOutputPremultiplication() const
{
    const std::string & str = ofxGetOutputPremultiplication();

    if (str == kOfxImagePreMultiplied) {
        return Natron::eImagePremultiplicationPremultiplied;
    } else if (str == kOfxImageUnPreMultiplied) {
        return Natron::eImagePremultiplicationUnPremultiplied;
    } else {
        return Natron::eImagePremultiplicationOpaque;
    }
}

const std::string &
OfxEffectInstance::ofxGetOutputPremultiplication() const
{
    static const std::string v(kOfxImagePreMultiplied);
    OFX::Host::ImageEffect::ClipInstance* clip = effectInstance()->getClip(kOfxImageEffectOutputClipName);

    assert(clip);
    
    if (getRecursionLevel() > 0) {
        const std::string & premult = effectInstance()->getOutputPreMultiplication();
        ///if the output has something, use it, otherwise default to premultiplied
        if ( !premult.empty() ) {
            return premult;
        } else {
            return v;
        }
    } else {
        ///Take the preferences lock to be sure we're not writing them
        QReadLocker l(_preferencesLock);
        const std::string & premult = effectInstance()->getOutputPreMultiplication();
        ///if the output has something, use it, otherwise default to premultiplied
        if ( !premult.empty() ) {
            return premult;
        } else {
            return v;
        }
    }
}


double
OfxEffectInstance::getPreferredAspectRatio() const
{
    OFX::Host::ImageEffect::ClipInstance* clip = effectInstance()->getClip(kOfxImageEffectOutputClipName);
    assert(clip);
    
    if (getRecursionLevel() > 0) {
        return clip->getAspectRatio();
    } else {
        ///Take the preferences lock to be sure we're not writing them
        QReadLocker l(_preferencesLock);
        return clip->getAspectRatio();

    }
}

double
OfxEffectInstance::getPreferredFrameRate() const
{
    OFX::Host::ImageEffect::ClipInstance* clip = effectInstance()->getClip(kOfxImageEffectOutputClipName);
    assert(clip);
    
    if (getRecursionLevel() > 0) {
        return clip->getFrameRate();
    } else {
        ///Take the preferences lock to be sure we're not writing them
        QReadLocker l(_preferencesLock);
        return clip->getFrameRate();
        
    }
}


bool
OfxEffectInstance::getCanTransform() const
{   //use OFX_EXTENSIONS_NUKE
    return effectInstance()->canTransform();
}

bool
OfxEffectInstance::getCanApplyTransform(Natron::EffectInstance** effect) const
{
    OfxClipInstance* transformClip = 0;
    bool canApply = effectInstance()->getCanApplyTransform(&transformClip);
    if (!transformClip || !canApply) {
        return false;
    }
    
    *effect = transformClip->getAssociatedNode();
    return true;
}

Natron::StatusEnum
OfxEffectInstance::getTransform(SequenceTime time,
                                const RenderScale& renderScale, //< the plug-in accepted scale
                                int view,
                                Natron::EffectInstance** inputToTransform,
                                Transform::Matrix3x3* transform)
{
    assert(getCanTransform());
    const std::string field = kOfxImageFieldNone; // TODO: support interlaced data
    
    std::string clipName;
    double tmpTransform[9];
    
    OfxStatus stat ;
    {
        bool skipDiscarding = false;
        if (getRecursionLevel() > 1) {
            skipDiscarding = true;
        }
        
        SET_CAN_SET_VALUE(false);
        
        
        ClipsThreadStorageSetter clipSetter(effectInstance(),
                                            skipDiscarding,
                                            true, //< setView ?
                                            view,
                                            true,//< set mipmaplevel ?
                                            Natron::Image::getLevelFromScale(renderScale.x));
        
        
        stat = effectInstance()->getTransformAction((OfxTime)time, field, renderScale, view, clipName, tmpTransform);
        if (stat == kOfxStatReplyDefault) {
            return Natron::eStatusReplyDefault;
        } else if (stat == kOfxStatFailed) {
            return Natron::eStatusFailed;
        }

    }

    
    assert(stat == kOfxStatOK);
    
    transform->a = tmpTransform[0]; transform->b = tmpTransform[1]; transform->c = tmpTransform[2];
    transform->d = tmpTransform[3]; transform->e = tmpTransform[4]; transform->f = tmpTransform[5];
    transform->g = tmpTransform[6]; transform->h = tmpTransform[7]; transform->i = tmpTransform[8];
    
    
    
    OFX::Host::ImageEffect::ClipInstance* clip = effectInstance()->getClip(clipName);
    assert(clip);
    OfxClipInstance* natronClip = dynamic_cast<OfxClipInstance*>(clip);
    if (!natronClip) {
        return Natron::eStatusFailed;
    }
    *inputToTransform = natronClip->getAssociatedNode();
    if (!*inputToTransform) {
        return Natron::eStatusFailed;
    }
    return Natron::eStatusOK;
}

void
OfxEffectInstance::rerouteInputAndSetTransform(int inputNb,Natron::EffectInstance* newInput,
                                               int newInputNb,const Transform::Matrix3x3& m)
{
    OfxClipInstance* clip = getClipCorrespondingToInput(inputNb);
    assert(clip);
    clip->setTransformAndReRouteInput(m, newInput, newInputNb);
}

void
OfxEffectInstance::clearTransform(int inputNb)
{
    OfxClipInstance* clip = getClipCorrespondingToInput(inputNb);
    assert(clip);
    clip->clearTransform();
}

bool
OfxEffectInstance::isFrameVarying() const
{
    return effectInstance()->isFrameVarying();
}

bool
OfxEffectInstance::doesTemporalClipAccess() const
{
    return effectInstance()->temporalAccess();
}

