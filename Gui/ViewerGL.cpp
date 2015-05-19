//  Natron
/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */
/*
 * Created by Alexandre GAUTHIER-FOICHAT on 6/1/2012.
 * contact: immarespond at gmail dot com
 *
 */

#include "ViewerGL.h"

#include <cassert>
#include <map>

#include <QtCore/QEvent>
#include <QtCore/QFile>
#include <QtCore/QHash>
#include <QtCore/QMutex>
#include <QtCore/QWaitCondition>
#include <QtGui/QPainter>
#include <QtGui/QImage>
#include <QToolButton>
#include <QApplication>
#include <QDesktopWidget>
#include <QScreen>
#include <QMenu> // in QtGui on Qt4, in QtWidgets on Qt5
#include <QDockWidget> // in QtGui on Qt4, in QtWidgets on Qt5
#include <QtGui/QPainter>
CLANG_DIAG_OFF(unused-private-field)
// /opt/local/include/QtGui/qmime.h:119:10: warning: private field 'type' is not used [-Wunused-private-field]
#include <QtGui/QKeyEvent>
CLANG_DIAG_ON(unused-private-field)
#include <QtGui/QVector4D>
#include <QtOpenGL/QGLShaderProgram>

#include "Global/Macros.h"

#include "Engine/Format.h"
#include "Engine/FrameEntry.h"
#include "Engine/Image.h"
#include "Engine/ImageInfo.h"
#include "Engine/Lut.h"
#include "Engine/MemoryFile.h"
#include "Engine/Settings.h"
#include "Engine/Timer.h"
#include "Engine/ViewerInstance.h"
#include "Engine/Project.h"
#include "Engine/Node.h"

#include "Gui/GuiApplicationManager.h"
#include "Gui/GuiAppInstance.h"
#include "Gui/Gui.h"
#include "Gui/InfoViewerWidget.h"
#include "Gui/Texture.h"
#include "Gui/Shaders.h"
#include "Gui/SpinBox.h"
#include "Gui/TabWidget.h"
#include "Gui/TextRenderer.h"
#include "Gui/TimeLineGui.h"
#include "Gui/ViewerTab.h"
#include "Gui/ProjectGui.h"
#include "Gui/ZoomContext.h"
#include "Gui/GuiMacros.h"
#include "Gui/ActionShortcuts.h"
#include "Gui/NodeGraph.h"
#include "Gui/CurveWidget.h"
#include "Gui/Histogram.h"

// warning: 'gluErrorString' is deprecated: first deprecated in OS X 10.9 [-Wdeprecated-declarations]
CLANG_DIAG_OFF(deprecated-declarations)
GCC_DIAG_OFF(deprecated-declarations)

#define USER_ROI_BORDER_TICK_SIZE 15.f
#define USER_ROI_CROSS_RADIUS 15.f
#define USER_ROI_SELECTION_POINT_SIZE 8.f
#define USER_ROI_CLICK_TOLERANCE 8.f

#define WIPE_MIX_HANDLE_LENGTH 50.
#define WIPE_ROTATE_HANDLE_LENGTH 100.
#define WIPE_ROTATE_OFFSET 30

#define PERSISTENT_MESSAGE_LEFT_OFFSET_PIXELS 20

#ifndef M_PI
#define M_PI        3.14159265358979323846264338327950288   /* pi             */
#endif

#ifndef M_LN2
#define M_LN2       0.693147180559945309417232121458176568  /* loge(2)        */
#endif


#define MAX_MIP_MAP_LEVELS 20
/*This class is the the core of the viewer : what displays images, overlays, etc...
   Everything related to OpenGL will (almost always) be in this class */

//using namespace Imf;
//using namespace Imath;
using namespace Natron;
using std::cout; using std::endl;

namespace {
/**
 *@enum MouseStateEnum
 *@brief basic state switching for mouse events
 **/
enum MouseStateEnum
{
    eMouseStateSelecting = 0,
    eMouseStateDraggingImage,
    eMouseStateDraggingRoiLeftEdge,
    eMouseStateDraggingRoiRightEdge,
    eMouseStateDraggingRoiTopEdge,
    eMouseStateDraggingRoiBottomEdge,
    eMouseStateDraggingRoiTopLeft,
    eMouseStateDraggingRoiTopRight,
    eMouseStateDraggingRoiBottomRight,
    eMouseStateDraggingRoiBottomLeft,
    eMouseStateDraggingRoiCross,
    eMouseStatePickingColor,
    eMouseStateBuildingPickerRectangle,
    eMouseStateDraggingWipeCenter,
    eMouseStateDraggingWipeMixHandle,
    eMouseStateRotatingWipeHandle,
    eMouseStateZoomingImage,
    eMouseStateUndefined
};

enum HoverStateEnum
{
    eHoverStateNothing = 0,
    eHoverStateWipeMix,
    eHoverStateWipeRotateHandle
};
} // namespace

enum PickerStateEnum
{
    ePickerStateInactive = 0,
    ePickerStatePoint,
    ePickerStateRectangle
};

struct ViewerGL::Implementation
{
    Implementation(ViewerTab* parent,
                   ViewerGL* _this)
        : pboIds()
          , vboVerticesId(0)
          , vboTexturesId(0)
          , iboTriangleStripId(0)
          , activeTextures()
          , displayTextures()
          , shaderRGB(0)
          , shaderBlack(0)
          , shaderLoaded(false)
          , infoViewer()
          , viewerTab(parent)
          , zoomOrPannedSinceLastFit(false)
          , oldClick()
          , blankViewerInfo()
          , displayingImageGain()
          , displayingImageOffset()
          , displayingImageMipMapLevel()
          , displayingImagePremult()
          , displayingImageLut(Natron::eViewerColorSpaceSRGB)
          , ms(eMouseStateUndefined)
          , hs(eHoverStateNothing)
          , textRenderingColor(200,200,200,255)
          , displayWindowOverlayColor(125,125,125,255)
          , rodOverlayColor(100,100,100,255)
          , textFont( new QFont(appFont,appFontSize) )
          , overlay(true)
          , supportsGLSL(true)
          , updatingTexture(false)
          , clearColor(0,0,0,255)
          , menu( new QMenu(_this) )
          , persistentMessages()
          , persistentMessageType(0)
          , displayPersistentMessage(false)
          , textRenderer()
          , isUserRoISet(false)
          , lastMousePosition()
          , lastDragStartPos()
          , hasMovedSincePress(false)
          , currentViewerInfo()
          , projectFormatMutex()
          , projectFormat()
          , currentViewerInfo_btmLeftBBOXoverlay()
          , currentViewerInfo_topRightBBOXoverlay()
          , currentViewerInfo_resolutionOverlay()
          , pickerState(ePickerStateInactive)
          , lastPickerPos()
          , userRoIEnabled(false) // protected by mutex
          , userRoI() // protected by mutex
          , zoomCtx() // protected by mutex
          , clipToDisplayWindow(true) // protected by mutex
          , wipeControlsMutex()
          , mixAmount(1.) // protected by mutex
          , wipeAngle(M_PI / 2.) // protected by mutex
          , wipeCenter()
          , selectionRectangle()
          , checkerboardTextureID(0)
          , checkerboardTileSize(0)
          , savedTexture(0)
          , prevBoundTexture(0)
          , lastRenderedImageMutex()
          , lastRenderedImage()
          , memoryHeldByLastRenderedImages()
          , sizeH()
    {
        infoViewer[0] = 0;
        infoViewer[1] = 0;
        displayTextures[0] = 0;
        displayTextures[1] = 0;
        activeTextures[0] = 0;
        activeTextures[1] = 0;
        memoryHeldByLastRenderedImages[0] = memoryHeldByLastRenderedImages[1] = 0;
        displayingImageGain[0] = displayingImageGain[1] = 1.;
        displayingImageOffset[0] = displayingImageOffset[1] = 0.;
        for (int i = 0; i < 2 ; ++i) {
            displayingImageTime[i] = 0;
            displayingImageMipMapLevel[i] = 0;
            lastRenderedImage[i].resize(MAX_MIP_MAP_LEVELS);
        }
        assert( qApp && qApp->thread() == QThread::currentThread() );
        menu->setFont( QFont(appFont,appFontSize) );
        
//        QDesktopWidget* desktop = QApplication::desktop();
//        QRect r = desktop->screenGeometry();
//        sizeH = r.size();
        sizeH.setWidth(10000);
        sizeH.setHeight(10000);
    }

    /////////////////////////////////////////////////////////
    // The following are only accessed from the main thread:

    std::vector<GLuint> pboIds; //!< PBO's id's used by the OpenGL context
    //   GLuint vaoId; //!< VAO holding the rendering VBOs for texture mapping.
    GLuint vboVerticesId; //!< VBO holding the vertices for the texture mapping.
    GLuint vboTexturesId; //!< VBO holding texture coordinates.
    GLuint iboTriangleStripId; /*!< IBOs holding vertices indexes for triangle strip sets*/
    Texture* activeTextures[2]; /*!< A pointer to the current textures used to display. One for A and B. May point to blackTex */
    Texture* displayTextures[2]; /*!< A pointer to the textures that would be used if A and B are displayed*/
    QGLShaderProgram* shaderRGB; /*!< The shader program used to render RGB data*/
    QGLShaderProgram* shaderBlack; /*!< The shader program used when the viewer is disconnected.*/
    bool shaderLoaded; /*!< Flag to check whether the shaders have already been loaded.*/
    InfoViewerWidget* infoViewer[2]; /*!< Pointer to the info bar below the viewer holding pixel/mouse/format related info*/
    ViewerTab* const viewerTab; /*!< Pointer to the viewer tab GUI*/
    bool zoomOrPannedSinceLastFit; //< true if the user zoomed or panned the image since the last call to fitToRoD
    QPoint oldClick;
    ImageInfo blankViewerInfo; /*!< Pointer to the info used when the viewer is disconnected.*/
    double displayingImageGain[2];
    double displayingImageOffset[2];
    unsigned int displayingImageMipMapLevel[2];
    Natron::ImagePremultiplicationEnum displayingImagePremult[2];
    int displayingImageTime[2];
    Natron::ViewerColorSpaceEnum displayingImageLut;
    MouseStateEnum ms; /*!< Holds the mouse state*/
    HoverStateEnum hs;
    const QColor textRenderingColor;
    const QColor displayWindowOverlayColor;
    const QColor rodOverlayColor;
    QFont* textFont;
    bool overlay; /*!< True if the user enabled overlay dispay*/

    // supportsGLSL is accessed from several threads, but is set only once at startup
    bool supportsGLSL; /*!< True if the user has a GLSL version supporting everything requested.*/
    bool updatingTexture;
    QColor clearColor;
    QMenu* menu;
    QStringList persistentMessages;
    int persistentMessageType;
    bool displayPersistentMessage;
    Natron::TextRenderer textRenderer;
    bool isUserRoISet;
    QPoint lastMousePosition; //< in widget coordinates
    QPointF lastDragStartPos; //< in zoom coordinates
    bool hasMovedSincePress;

    /////// currentViewerInfo
    ImageInfo currentViewerInfo[2]; /*!< Pointer to the ViewerInfo  used for rendering*/
    
    mutable QMutex projectFormatMutex;
    Format projectFormat;
    QString currentViewerInfo_btmLeftBBOXoverlay[2]; /*!< The string holding the bottom left corner coordinates of the dataWindow*/
    QString currentViewerInfo_topRightBBOXoverlay[2]; /*!< The string holding the top right corner coordinates of the dataWindow*/
    QString currentViewerInfo_resolutionOverlay; /*!< The string holding the resolution overlay, e.g: "1920x1080"*/

    ///////Picker info, used only by the main-thread
    PickerStateEnum pickerState;
    QPointF lastPickerPos;
    QRectF pickerRect;

    //////////////////////////////////////////////////////////
    // The following are accessed from various threads
    QMutex userRoIMutex;
    bool userRoIEnabled;
    RectD userRoI; //< in canonical coords
    ZoomContext zoomCtx; /*!< All zoom related variables are packed into this object. */
    mutable QMutex zoomCtxMutex; /// protectx zoomCtx*
    QMutex clipToDisplayWindowMutex;
    bool clipToDisplayWindow;
    mutable QMutex wipeControlsMutex;
    double mixAmount; /// the amount of the second input to blend, by default 1.
    double wipeAngle; /// the angle to the X axis
    QPointF wipeCenter; /// the center of the wipe control
    QRectF selectionRectangle;
    
    GLuint checkerboardTextureID;
    int checkerboardTileSize; // to avoid a call to getValue() of the settings at each draw

    GLuint savedTexture; // @see saveOpenGLContext/restoreOpenGLContext
    GLuint prevBoundTexture; // @see bindTextureAndActivateShader/unbindTextureAndReleaseShader

    mutable QMutex lastRenderedImageMutex; //protects lastRenderedImage & memoryHeldByLastRenderedImages
    std::vector<boost::shared_ptr<Natron::Image> > lastRenderedImage[2]; //<  last image passed to transferRAMBuffer
    U64 memoryHeldByLastRenderedImages[2];
    
    QSize sizeH;
    
    bool isNearbyWipeCenter(const QPointF & pos,double zoomScreenPixelWidth, double zoomScreenPixelHeight ) const;
    bool isNearbyWipeRotateBar(const QPointF & pos,double zoomScreenPixelWidth, double zoomScreenPixelHeight) const;
    bool isNearbyWipeMixHandle(const QPointF & pos,double zoomScreenPixelWidth, double zoomScreenPixelHeight) const;

    void drawArcOfCircle(const QPointF & center,double radiusX,double radiusY,double startAngle,double endAngle);

    void bindTextureAndActivateShader(int i,
                                      bool useShader)
    {
        assert(activeTextures[i]);
        glActiveTexture(GL_TEXTURE0);
        glGetIntegerv(GL_TEXTURE_BINDING_2D, (GLint*)&prevBoundTexture);
        glBindTexture( GL_TEXTURE_2D, activeTextures[i]->getTexID() );
        // debug (so the OpenGL debugger can make a breakpoint here)
        //GLfloat d;
        //glReadPixels(0, 0, 1, 1, GL_RED, GL_FLOAT, &d);
        if (useShader) {
            activateShaderRGB(i);
        }
        glCheckError();
    }

    void unbindTextureAndReleaseShader(bool useShader)
    {
        if (useShader) {
            shaderRGB->release();
        }
        glCheckError();
        glBindTexture(GL_TEXTURE_2D, prevBoundTexture);
    }

    /**
     *@brief Starts using the RGB shader to display the frame
     **/
    void activateShaderRGB(int texIndex);

    enum WipePolygonEnum
    {
        eWipePolygonEmpty = 0,  // don't draw anything
        eWipePolygonFull,  // draw the whole texture as usual
        eWipePolygonPartial, // use the polygon returned to draw
    };

    WipePolygonEnum getWipePolygon(const RectD & texRectClipped, //!< in canonical coordinates
                                   QPolygonF & polygonPoints,
                                   bool rightPlane) const;

    static void getBaseTextureCoordinates(const RectI & texRect,int closestPo2,int texW,int texH,
                                          GLfloat & bottom,GLfloat & top,GLfloat & left,GLfloat & right);
    static void getPolygonTextureCoordinates(const QPolygonF & polygonPoints,
                                             const RectD & texRect, //!< in canonical coordinates
                                             QPolygonF & texCoords);

    void refreshSelectionRectangle(const QPointF & pos);

    void drawSelectionRectangle();
    
    void initializeCheckerboardTexture(bool mustCreateTexture);
    
    void drawCheckerboardTexture(const RectD& rod);
    
    void getProjectFormatCanonical(RectD& canonicalProjectFormat) const
    {
        QMutexLocker k(&projectFormatMutex);
        canonicalProjectFormat = projectFormat.toCanonicalFormat();
    }
};

#if 0
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

#endif
//static const GLfloat renderingTextureCoordinates[32] = {
//    0 , 1 , //0
//    0 , 1 , //1
//    1 , 1 , //2
//    1 , 1 , //3
//    0 , 1 , //4
//    0 , 1 , //5
//    1 , 1 , //6
//    1 , 1 , //7
//    0 , 0 , //8
//    0 , 0 , //9
//    1 , 0 , //10
//    1 , 0 , //11
//    0 , 0 , //12
//    0 , 0 , //13
//    1 , 0 , //14
//    1 , 0   //15
//};

/*see http://www.learnopengles.com/android-lesson-eight-an-introduction-to-index-buffer-objects-ibos/ */
static const GLubyte triangleStrip[28] = {
    0,4,1,5,2,6,3,7,
    7,4,
    4,8,5,9,6,10,7,11,
    11,8,
    8,12,9,13,10,14,11,15
};

/*
   ASCII art of the vertices used to render.
   The actual texture seen on the viewport is the rect (5,9,10,6).
   We draw  3*6 strips

 0___1___2___3
 |  /|  /|  /|
 | / | / | / |
 |/  |/  |/  |
 4---5---6----7
 |  /|  /|  /|
 | / | / | / |
 |/  |/  |/  |
 8---9--10--11
 |  /|  /|  /|
 | / | / | / |
 |/  |/  |/  |
 12--13--14--15
 */


void
ViewerGL::drawRenderingVAO(unsigned int mipMapLevel,
                           int textureIndex,
                           ViewerGL::DrawPolygonModeEnum polygonMode)
{
    // always running in the main thread
    assert( qApp && qApp->thread() == QThread::currentThread() );
    assert( QGLContext::currentContext() == context() );

    bool useShader = getBitDepth() != OpenGLViewerI::eBitDepthByte && _imp->supportsGLSL;

    
    ///the texture rectangle in image coordinates. The values in it are multiples of tile size.
    ///
    const TextureRect &r = _imp->activeTextures[textureIndex]->getTextureRect();

    ///This is the coordinates in the image being rendered where datas are valid, this is in pixel coordinates
    ///at the time we initialize it but we will convert it later to canonical coordinates. See 1)
    RectI texRect(r.x1,r.y1,r.x2,r.y2);

    const double par = r.par;
    
    RectD canonicalTexRect;
    texRect.toCanonical_noClipping(mipMapLevel,par/*, rod*/, &canonicalTexRect);

    ///the RoD of the image in canonical coords.
    RectD rod = getRoD(textureIndex);

    bool clipToDisplayWindow;
    {
        QMutexLocker l(&_imp->clipToDisplayWindowMutex);
        clipToDisplayWindow = _imp->clipToDisplayWindow;
    }
    
    RectD rectClippedToRoI(canonicalTexRect);

    if (clipToDisplayWindow) {
        RectD canonicalProjectFormat;
        _imp->getProjectFormatCanonical(canonicalProjectFormat);
        rod.intersect(canonicalProjectFormat, &rod);
        rectClippedToRoI.intersect(canonicalProjectFormat, &rectClippedToRoI);
    }
    
    
    //if user RoI is enabled, clip the rod to that roi
    bool userRoiEnabled;
    {
        QMutexLocker l(&_imp->userRoIMutex);
        userRoiEnabled = _imp->userRoIEnabled;
    }


    ////The texture real size (r.w,r.h) might be slightly bigger than the actual
    ////pixel coordinates bounds r.x1,r.x2 r.y1 r.y2 because we clipped these bounds against the bounds
    ////in the ViewerInstance::renderViewer function. That means we need to draw actually only the part of
    ////the texture that contains the bounds.
    ////Notice that r.w and r.h are scaled to the closest Po2 of the current scaling factor, so we need to scale it up
    ////So it is in the same coordinates as the bounds.
    ///Edit: we no longer divide by the closestPo2 since the viewer now computes images at lower resolution by itself, the drawing
    ///doesn't need to be scaled.
   
    if (userRoiEnabled) {
        {
            QMutexLocker l(&_imp->userRoIMutex);
            //if the userRoI isn't intersecting the rod, just don't render anything
            if ( !rod.intersect(_imp->userRoI,&rod) ) {
                return;
            }
        }
        rectClippedToRoI.intersect(rod, &rectClippedToRoI);
        //clipTexCoords<RectD>(canonicalTexRect,rectClippedToRoI,texBottom,texTop,texLeft,texRight);
    }

    if (polygonMode != eDrawPolygonModeWhole) {
        /// draw only  the plane defined by the wipe handle
        QPolygonF polygonPoints,polygonTexCoords;
        RectD floatRectClippedToRoI;
        floatRectClippedToRoI.x1 = rectClippedToRoI.x1;
        floatRectClippedToRoI.y1 = rectClippedToRoI.y1;
        floatRectClippedToRoI.x2 = rectClippedToRoI.x2;
        floatRectClippedToRoI.y2 = rectClippedToRoI.y2;
        Implementation::WipePolygonEnum polyType = _imp->getWipePolygon(floatRectClippedToRoI, polygonPoints, polygonMode == eDrawPolygonModeWipeRight);

        if (polyType == Implementation::eWipePolygonEmpty) {
            ///don't draw anything
            return;
        } else if (polyType == Implementation::eWipePolygonPartial) {
            _imp->getPolygonTextureCoordinates(polygonPoints, canonicalTexRect, polygonTexCoords);

            _imp->bindTextureAndActivateShader(textureIndex, useShader);

            glBegin(GL_POLYGON);
            for (int i = 0; i < polygonTexCoords.size(); ++i) {
                const QPointF & tCoord = polygonTexCoords[i];
                const QPointF & vCoord = polygonPoints[i];
                glTexCoord2d( tCoord.x(), tCoord.y() );
                glVertex2d( vCoord.x(), vCoord.y() );
            }
            glEnd();
            
            _imp->unbindTextureAndReleaseShader(useShader);

        } else {
            ///draw the all polygon as usual
            polygonMode = eDrawPolygonModeWhole;
        }
    }

    if (polygonMode == eDrawPolygonModeWhole) {
        
        
        
        ///Vertices are in canonical coords
        GLfloat vertices[32] = {
            (GLfloat)rod.left(),(GLfloat)rod.top(),    //0
            (GLfloat)rectClippedToRoI.x1, (GLfloat)rod.top(),          //1
            (GLfloat)rectClippedToRoI.x2, (GLfloat)rod.top(),    //2
            (GLfloat)rod.right(),(GLfloat)rod.top(),   //3
            (GLfloat)rod.left(), (GLfloat)rectClippedToRoI.y2, //4
            (GLfloat)rectClippedToRoI.x1,  (GLfloat)rectClippedToRoI.y2,       //5
            (GLfloat)rectClippedToRoI.x2,  (GLfloat)rectClippedToRoI.y2, //6
            (GLfloat)rod.right(),(GLfloat)rectClippedToRoI.y2, //7
            (GLfloat)rod.left(),(GLfloat)rectClippedToRoI.y1,        //8
            (GLfloat)rectClippedToRoI.x1,  (GLfloat)rectClippedToRoI.y1,             //9
            (GLfloat)rectClippedToRoI.x2,  (GLfloat)rectClippedToRoI.y1,       //10
            (GLfloat)rod.right(),(GLfloat)rectClippedToRoI.y1,       //11
            (GLfloat)rod.left(), (GLfloat)rod.bottom(), //12
            (GLfloat)rectClippedToRoI.x1,  (GLfloat)rod.bottom(),       //13
            (GLfloat)rectClippedToRoI.x2,  (GLfloat)rod.bottom(), //14
            (GLfloat)rod.right(),(GLfloat)rod.bottom() //15
        };
        
//        GLfloat texBottom =  0;
//        GLfloat texTop =  (GLfloat)(r.y2 - r.y1)  / (GLfloat)(r.h /** r.closestPo2*/);
//        GLfloat texLeft = 0;
//        GLfloat texRight = (GLfloat)(r.x2 - r.x1)  / (GLfloat)(r.w /** r.closestPo2*/);
        GLfloat texBottom = (GLfloat)(rectClippedToRoI.y1 - canonicalTexRect.y1)  / canonicalTexRect.height();
        GLfloat texTop = (GLfloat)(rectClippedToRoI.y2 - canonicalTexRect.y1)  / canonicalTexRect.height();
        GLfloat texLeft = (GLfloat)(rectClippedToRoI.x1 - canonicalTexRect.x1)  / canonicalTexRect.width();
        GLfloat texRight = (GLfloat)(rectClippedToRoI.x2 - canonicalTexRect.x1)  / canonicalTexRect.width();

        
        GLfloat renderingTextureCoordinates[32] = {
            texLeft, texTop,   //0
            texLeft, texTop,   //1
            texRight, texTop,  //2
            texRight, texTop,   //3
            texLeft, texTop,   //4
            texLeft, texTop,   //5
            texRight, texTop,   //6
            texRight, texTop,   //7
            texLeft, texBottom,   //8
            texLeft, texBottom,   //9
            texRight, texBottom,    //10
            texRight, texBottom,   //11
            texLeft, texBottom,   // 12
            texLeft, texBottom,   //13
            texRight, texBottom,   //14
            texRight, texBottom    //15
        };

        
        if (_imp->viewerTab->isCheckerboardEnabled()) {
            _imp->drawCheckerboardTexture(rod);
        }
        
        _imp->bindTextureAndActivateShader(textureIndex, useShader);

        glCheckError();

        glBindBuffer(GL_ARRAY_BUFFER, _imp->vboVerticesId);
        glBufferSubData(GL_ARRAY_BUFFER, 0, 32 * sizeof(GLfloat), vertices);
        glEnableClientState(GL_VERTEX_ARRAY);
        glVertexPointer(2, GL_FLOAT, 0, 0);

        glBindBuffer(GL_ARRAY_BUFFER, _imp->vboTexturesId);
        glBufferSubData(GL_ARRAY_BUFFER, 0, 32 * sizeof(GLfloat), renderingTextureCoordinates);
        glClientActiveTexture(GL_TEXTURE0);
        glEnableClientState(GL_TEXTURE_COORD_ARRAY);
        glTexCoordPointer(2, GL_FLOAT, 0, 0);

        glDisableClientState(GL_COLOR_ARRAY);

        glBindBuffer(GL_ARRAY_BUFFER,0);

        glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, _imp->iboTriangleStripId);
        glDrawElements(GL_TRIANGLE_STRIP, 28, GL_UNSIGNED_BYTE, 0);
        glCheckErrorIgnoreOSXBug();

        glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, 0);
        glDisableClientState(GL_VERTEX_ARRAY);
        glDisableClientState(GL_TEXTURE_COORD_ARRAY);
        glCheckError();
        
        _imp->unbindTextureAndReleaseShader(useShader);
    }
} // drawRenderingVAO

void
ViewerGL::Implementation::getBaseTextureCoordinates(const RectI & r,
                                                    int closestPo2,
                                                    int texW,
                                                    int texH,
                                                    GLfloat & bottom,
                                                    GLfloat & top,
                                                    GLfloat & left,
                                                    GLfloat & right)
{
    bottom =  0;
    top =  (GLfloat)(r.y2 - r.y1)  / (GLfloat)(texH * closestPo2);
    left = 0;
    right = (GLfloat)(r.x2 - r.x1)  / (GLfloat)(texW * closestPo2);
}

void
ViewerGL::Implementation::getPolygonTextureCoordinates(const QPolygonF & polygonPoints,
                                                       const RectD & texRect,
                                                       QPolygonF & texCoords)
{
    texCoords.resize( polygonPoints.size() );
    for (int i = 0; i < polygonPoints.size(); ++i) {
        const QPointF & polygonPoint = polygonPoints.at(i);
        QPointF texCoord;
        texCoord.setX( (polygonPoint.x() - texRect.x1) / texRect.width() ); // * (right - left));
        texCoord.setY( (polygonPoint.y() - texRect.y1) / texRect.height() ); // * (top - bottom));
        texCoords[i] = texCoord;
    }
}

ViewerGL::Implementation::WipePolygonEnum
ViewerGL::Implementation::getWipePolygon(const RectD & texRectClipped,
                                         QPolygonF & polygonPoints,
                                         bool rightPlane) const
{
    ///Compute a second point on the plane separator line
    ///we don't really care how far it is from the center point, it just has to be on the line
    QPointF firstPoint,secondPoint;
    double mpi2 = M_PI / 2.;
    QPointF center;
    double angle;
    {
        QMutexLocker l(&wipeControlsMutex);
        center = wipeCenter;
        angle = wipeAngle;
    }

    ///extrapolate the line to the maximum size of the RoD so we're sure the line
    ///intersection algorithm works
    double maxSize = std::max(texRectClipped.x2 - texRectClipped.x1,texRectClipped.y2 - texRectClipped.y1) * 10000.;
    double xmax,ymax;

    xmax = std::cos(angle + mpi2) * maxSize;
    ymax = std::sin(angle + mpi2) * maxSize;

    firstPoint.setX(center.x() - xmax);
    firstPoint.setY(center.y() - ymax);
    secondPoint.setX(center.x() + xmax);
    secondPoint.setY(center.y() + ymax);

    QLineF inter(firstPoint,secondPoint);
    QLineF::IntersectType intersectionTypes[4];
    QPointF intersections[4];
    QLineF topEdge(texRectClipped.x1,texRectClipped.y2,texRectClipped.x2,texRectClipped.y2);
    QLineF rightEdge(texRectClipped.x2,texRectClipped.y2,texRectClipped.x2,texRectClipped.y1);
    QLineF bottomEdge(texRectClipped.x2,texRectClipped.y1,texRectClipped.x1,texRectClipped.y1);
    QLineF leftEdge(texRectClipped.x1,texRectClipped.y1,texRectClipped.x1,texRectClipped.y2);
    bool crossingTop = false,crossingRight = false,crossingLeft = false,crossingBtm = false;
    int validIntersectionsIndex[4];
    validIntersectionsIndex[0] = validIntersectionsIndex[1] = -1;
    int numIntersec = 0;
    intersectionTypes[0] = inter.intersect(topEdge, &intersections[0]);
    if (intersectionTypes[0] == QLineF::BoundedIntersection) {
        validIntersectionsIndex[numIntersec] = 0;
        crossingTop = true;
        ++numIntersec;
    }
    intersectionTypes[1] = inter.intersect(rightEdge, &intersections[1]);
    if (intersectionTypes[1]  == QLineF::BoundedIntersection) {
        validIntersectionsIndex[numIntersec] = 1;
        crossingRight = true;
        ++numIntersec;
    }
    intersectionTypes[2] = inter.intersect(bottomEdge, &intersections[2]);
    if (intersectionTypes[2]  == QLineF::BoundedIntersection) {
        validIntersectionsIndex[numIntersec] = 2;
        crossingBtm = true;
        ++numIntersec;
    }
    intersectionTypes[3] = inter.intersect(leftEdge, &intersections[3]);
    if (intersectionTypes[3]  == QLineF::BoundedIntersection) {
        validIntersectionsIndex[numIntersec] = 3;
        crossingLeft = true;
        ++numIntersec;
    }

    if ( (numIntersec != 0) && (numIntersec != 2) ) {
        ///Don't bother drawing the polygon, it is most certainly not visible in this case
        return ViewerGL::Implementation::eWipePolygonEmpty;
    }

    ///determine the orientation of the planes
    double crossProd  = ( secondPoint.x() - center.x() ) * ( texRectClipped.y1 - center.y() )
                        - ( secondPoint.y() - center.y() ) * ( texRectClipped.x1 - center.x() );
    if (numIntersec == 0) {
        ///the bottom left corner is on the left plane
        if ( (crossProd > 0) && ( (center.x() >= texRectClipped.x2) || (center.y() >= texRectClipped.y2) ) ) {
            ///the plane is invisible because the wipe handle is below or on the left of the texRectClipped
            return rightPlane ? ViewerGL::Implementation::eWipePolygonEmpty : ViewerGL::Implementation::eWipePolygonFull;
        }

        ///otherwise we draw the entire texture as usual
        return rightPlane ? ViewerGL::Implementation::eWipePolygonFull : ViewerGL::Implementation::eWipePolygonEmpty;
    } else {
        ///we have 2 intersects
        assert(validIntersectionsIndex[0] != -1 && validIntersectionsIndex[1] != -1);
        bool isBottomLeftOnLeftPlane = crossProd > 0;

        ///there are 6 cases
        if (crossingBtm && crossingLeft) {
            if ( (isBottomLeftOnLeftPlane && rightPlane) || (!isBottomLeftOnLeftPlane && !rightPlane) ) {
                //btm intersect is the first
                polygonPoints.insert(0,intersections[validIntersectionsIndex[0]]);
                polygonPoints.insert(1,intersections[validIntersectionsIndex[1]]);
                polygonPoints.insert( 2,QPointF(texRectClipped.x1,texRectClipped.y2) );
                polygonPoints.insert( 3,QPointF(texRectClipped.x2,texRectClipped.y2) );
                polygonPoints.insert( 4,QPointF(texRectClipped.x2,texRectClipped.y1) );
                polygonPoints.insert(5,intersections[validIntersectionsIndex[0]]);
            } else {
                polygonPoints.insert(0,intersections[validIntersectionsIndex[0]]);
                polygonPoints.insert(1,intersections[validIntersectionsIndex[1]]);
                polygonPoints.insert( 2,QPointF(texRectClipped.x1,texRectClipped.y1) );
                polygonPoints.insert(3,intersections[validIntersectionsIndex[0]]);
            }
        } else if (crossingBtm && crossingTop) {
            if ( (isBottomLeftOnLeftPlane && rightPlane) || (!isBottomLeftOnLeftPlane && !rightPlane) ) {
                ///btm intersect is the second
                polygonPoints.insert(0,intersections[validIntersectionsIndex[1]]);
                polygonPoints.insert(1,intersections[validIntersectionsIndex[0]]);
                polygonPoints.insert( 2,QPointF(texRectClipped.x2,texRectClipped.y2) );
                polygonPoints.insert( 3,QPointF(texRectClipped.x2,texRectClipped.y1) );
                polygonPoints.insert(4,intersections[validIntersectionsIndex[1]]);
            } else {
                polygonPoints.insert(0,intersections[validIntersectionsIndex[1]]);
                polygonPoints.insert(1,intersections[validIntersectionsIndex[0]]);
                polygonPoints.insert( 2,QPointF(texRectClipped.x1,texRectClipped.y2) );
                polygonPoints.insert( 3,QPointF(texRectClipped.x1,texRectClipped.y1) );
                polygonPoints.insert(4,intersections[validIntersectionsIndex[1]]);
            }
        } else if (crossingBtm && crossingRight) {
            if ( (isBottomLeftOnLeftPlane && rightPlane) || (!isBottomLeftOnLeftPlane && !rightPlane) ) {
                ///btm intersect is the second
                polygonPoints.insert(0,intersections[validIntersectionsIndex[1]]);
                polygonPoints.insert(1,intersections[validIntersectionsIndex[0]]);
                polygonPoints.insert( 2,QPointF(texRectClipped.x2,texRectClipped.y1) );
                polygonPoints.insert(3,intersections[validIntersectionsIndex[1]]);
            } else {
                polygonPoints.insert(0,intersections[validIntersectionsIndex[1]]);
                polygonPoints.insert(1,intersections[validIntersectionsIndex[0]]);
                polygonPoints.insert( 2,QPointF(texRectClipped.x2,texRectClipped.y2) );
                polygonPoints.insert( 3,QPointF(texRectClipped.x1,texRectClipped.y2) );
                polygonPoints.insert( 4,QPointF(texRectClipped.x1,texRectClipped.y1) );
                polygonPoints.insert(5,intersections[validIntersectionsIndex[1]]);
            }
        } else if (crossingLeft && crossingTop) {
            if ( (isBottomLeftOnLeftPlane && rightPlane) || (!isBottomLeftOnLeftPlane && !rightPlane) ) {
                ///left intersect is the second
                polygonPoints.insert(0,intersections[validIntersectionsIndex[1]]);
                polygonPoints.insert(1,intersections[validIntersectionsIndex[0]]);
                polygonPoints.insert( 2,QPointF(texRectClipped.x1,texRectClipped.y2) );
                polygonPoints.insert(3,intersections[validIntersectionsIndex[1]]);
            } else {
                polygonPoints.insert(0,intersections[validIntersectionsIndex[1]]);
                polygonPoints.insert(1,intersections[validIntersectionsIndex[0]]);
                polygonPoints.insert( 2,QPointF(texRectClipped.x2,texRectClipped.y2) );
                polygonPoints.insert( 3,QPointF(texRectClipped.x2,texRectClipped.y1) );
                polygonPoints.insert( 4,QPointF(texRectClipped.x1,texRectClipped.y1) );
                polygonPoints.insert(5,intersections[validIntersectionsIndex[1]]);
            }
        } else if (crossingLeft && crossingRight) {
            if ( (isBottomLeftOnLeftPlane && rightPlane) || (!isBottomLeftOnLeftPlane && !rightPlane) ) {
                ///left intersect is the second
                polygonPoints.insert(0,intersections[validIntersectionsIndex[1]]);
                polygonPoints.insert( 1,QPointF(texRectClipped.x1,texRectClipped.y2) );
                polygonPoints.insert( 2,QPointF(texRectClipped.x2,texRectClipped.y2) );
                polygonPoints.insert(3,intersections[validIntersectionsIndex[0]]);
                polygonPoints.insert(4,intersections[validIntersectionsIndex[1]]);
            } else {
                polygonPoints.insert(0,intersections[validIntersectionsIndex[1]]);
                polygonPoints.insert(1,intersections[validIntersectionsIndex[0]]);
                polygonPoints.insert( 2,QPointF(texRectClipped.x2,texRectClipped.y1) );
                polygonPoints.insert( 3,QPointF(texRectClipped.x1,texRectClipped.y1) );
                polygonPoints.insert(4,intersections[validIntersectionsIndex[1]]);
            }
        } else if (crossingTop && crossingRight) {
            if ( (isBottomLeftOnLeftPlane && rightPlane) || (!isBottomLeftOnLeftPlane && !rightPlane) ) {
                ///right is second
                polygonPoints.insert(0,intersections[validIntersectionsIndex[0]]);
                polygonPoints.insert( 1,QPointF(texRectClipped.x2,texRectClipped.y2) );
                polygonPoints.insert(2,intersections[validIntersectionsIndex[1]]);
                polygonPoints.insert(3,intersections[validIntersectionsIndex[0]]);
            } else {
                polygonPoints.insert(0,intersections[validIntersectionsIndex[0]]);
                polygonPoints.insert(1,intersections[validIntersectionsIndex[1]]);
                polygonPoints.insert( 2,QPointF(texRectClipped.x2,texRectClipped.y1) );
                polygonPoints.insert( 3,QPointF(texRectClipped.x1,texRectClipped.y1) );
                polygonPoints.insert( 4,QPointF(texRectClipped.x1,texRectClipped.y2) );
                polygonPoints.insert(5,intersections[validIntersectionsIndex[0]]);
            }
        } else {
            assert(false);
        }
    }

    return ViewerGL::Implementation::eWipePolygonPartial;
} // getWipePolygon

ViewerGL::ViewerGL(ViewerTab* parent,
                   const QGLWidget* shareWidget)
    : QGLWidget(parent,shareWidget)
      , _imp( new Implementation(parent, this) )
{
    // always running in the main thread
    assert( qApp && qApp->thread() == QThread::currentThread() );
    setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);
    //setFocusPolicy(Qt::StrongFocus);
    setMouseTracking(true);

    QObject::connect( parent->getGui()->getApp()->getProject().get(),SIGNAL( formatChanged(Format) ),this,SLOT( onProjectFormatChanged(Format) ) );

    Format projectFormat;
    parent->getGui()->getApp()->getProject()->getProjectDefaultFormat(&projectFormat);

    RectD canonicalFormat = projectFormat.toCanonicalFormat();
    
    _imp->blankViewerInfo.setRoD(canonicalFormat);
    _imp->blankViewerInfo.setDisplayWindow(projectFormat);
    setRegionOfDefinition(_imp->blankViewerInfo.getRoD(),_imp->blankViewerInfo.getDisplayWindow().getPixelAspectRatio(),0);
    setRegionOfDefinition(_imp->blankViewerInfo.getRoD(),_imp->blankViewerInfo.getDisplayWindow().getPixelAspectRatio(),1);
    onProjectFormatChangedInternal(projectFormat,false);
    resetWipeControls();
    populateMenu();

    QObject::connect( appPTR, SIGNAL(checkerboardSettingsChanged()), this, SLOT(onCheckerboardSettingsChanged()));
}

ViewerGL::~ViewerGL()
{
    // always running in the main thread
    assert( qApp && qApp->thread() == QThread::currentThread() );
    makeCurrent();

    if (_imp->shaderRGB) {
        _imp->shaderRGB->removeAllShaders();
        delete _imp->shaderRGB;
    }
    if (_imp->shaderBlack) {
        _imp->shaderBlack->removeAllShaders();
        delete _imp->shaderBlack;
    }
    delete _imp->displayTextures[0];
    delete _imp->displayTextures[1];
    glCheckError();
    for (U32 i = 0; i < _imp->pboIds.size(); ++i) {
        glDeleteBuffers(1,&_imp->pboIds[i]);
    }
    glCheckError();
    glDeleteBuffers(1, &_imp->vboVerticesId);
    glDeleteBuffers(1, &_imp->vboTexturesId);
    glDeleteBuffers(1, &_imp->iboTriangleStripId);
    glCheckError();
    delete _imp->textFont;
    glDeleteTextures(1, &_imp->checkerboardTextureID);
}

QSize
ViewerGL::sizeHint() const
{
    // always running in the main thread
    assert( qApp && qApp->thread() == QThread::currentThread() );

    return _imp->sizeH;
}

const QFont &
ViewerGL::textFont() const
{
    // always running in the main thread
    assert( qApp && qApp->thread() == QThread::currentThread() );

    return *_imp->textFont;
}

void
ViewerGL::setTextFont(const QFont & f)
{
    // always running in the main thread
    assert( qApp && qApp->thread() == QThread::currentThread() );
    *_imp->textFont = f;
}

/**
 *@returns Returns true if the viewer is displaying something.
 **/
bool
ViewerGL::displayingImage() const
{
    // always running in the main thread
    assert( qApp && qApp->thread() == QThread::currentThread() );

    return _imp->activeTextures[0] != 0 || _imp->activeTextures[1] != 0;
}

void
ViewerGL::resizeGL(int width,
                   int height)
{
    // always running in the main thread
    assert( qApp && qApp->thread() == QThread::currentThread() );
    if ( (height == 0) || (width == 0) ) { // prevent division by 0
        return;
    }
    glViewport (0, 0, width, height);
    bool zoomSinceLastFit;
    int oldWidth,oldHeight;
    {
        QMutexLocker(&_imp->zoomCtxMutex);
        oldWidth = _imp->zoomCtx.screenWidth();
        oldHeight = _imp->zoomCtx.screenHeight();
        _imp->zoomCtx.setScreenSize(width, height);
        zoomSinceLastFit = _imp->zoomOrPannedSinceLastFit;
    }
    glCheckError();
    _imp->ms = eMouseStateUndefined;
    assert(_imp->viewerTab);
    ViewerInstance* viewer = _imp->viewerTab->getInternalNode();
    assert(viewer);
    if (!zoomSinceLastFit) {
        fitImageToFormat();
    }
    if ( viewer->getUiContext() && _imp->viewerTab->getGui() &&
         !_imp->viewerTab->getGui()->getApp()->getProject()->isLoadingProject() &&
         ( ( oldWidth != width) || ( oldHeight != height) ) ) {
        viewer->renderCurrentFrame(false);
        
        if (!_imp->persistentMessages.empty()) {
            updatePersistentMessageToWidth(width - 20);
        } else {
            updateGL();
        }
    }
}

/**
 * @brief Used to setup the blending mode to draw the first texture
 **/
class BlendSetter
{
    
    bool didBlend;
    
public:
    
    BlendSetter(ImagePremultiplicationEnum premult)
    {
        didBlend = premult != eImagePremultiplicationOpaque;
        if (didBlend) {
            glEnable(GL_BLEND);
        }
        switch (premult) {
            case Natron::eImagePremultiplicationPremultiplied:
                glBlendFunc(GL_ONE,GL_ONE_MINUS_SRC_ALPHA);
                break;
            case Natron::eImagePremultiplicationUnPremultiplied:
                glBlendFunc(GL_SRC_ALPHA,GL_ONE_MINUS_SRC_ALPHA);
                break;
            case Natron::eImagePremultiplicationOpaque:
                break;
        }

    }
    
    ~BlendSetter()
    {
        if (didBlend) {
            glDisable(GL_BLEND);
        }
    }
};

void
ViewerGL::paintGL()
{
    // always running in the main thread
    assert( qApp && qApp->thread() == QThread::currentThread() );

    //if app is closing, just return
    if ( !_imp->viewerTab->getGui() ) {
        return;
    }
    glCheckError();

    double zoomLeft, zoomRight, zoomBottom, zoomTop;
    {
        QMutexLocker l(&_imp->zoomCtxMutex);
        assert(0 < _imp->zoomCtx.factor() && _imp->zoomCtx.factor() <= 1024);
        zoomLeft = _imp->zoomCtx.left();
        zoomRight = _imp->zoomCtx.right();
        zoomBottom = _imp->zoomCtx.bottom();
        zoomTop = _imp->zoomCtx.top();
    }
    if ( (zoomLeft == zoomRight) || (zoomTop == zoomBottom) ) {
        clearColorBuffer( _imp->clearColor.redF(),_imp->clearColor.greenF(),_imp->clearColor.blueF(),_imp->clearColor.alphaF() );

        return;
    }

    {
        //GLProtectAttrib a(GL_TRANSFORM_BIT); // GL_MODELVIEW is active by default

        // Note: the OFX spec says that the GL_MODELVIEW should be the identity matrix
        // http://openfx.sourceforge.net/Documentation/1.3/ofxProgrammingReference.html#ImageEffectOverlays
        // However,
        // - Nuke uses a different matrix
        // - Nuke transforms the interacts using the modelview if there are Transform nodes between the viewer and the interact.


        glMatrixMode(GL_PROJECTION);
        glLoadIdentity();
        glOrtho(zoomLeft, zoomRight, zoomBottom, zoomTop, -1, 1);
        glScalef(256., 256., 1.0); // for compatibility with Nuke
        glTranslatef(1, 1, 0);     // for compatibility with Nuke
        glMatrixMode(GL_MODELVIEW);
        glLoadIdentity();
        glTranslatef(-1, -1, 0);        // for compatibility with Nuke
        glScalef(1/256., 1./256., 1.0); // for compatibility with Nuke

        glCheckError();


        // don't even bind the shader on 8-bits gamma-compressed textures
        ViewerCompositingOperatorEnum compOp = _imp->viewerTab->getCompositingOperator();

        ///Determine whether we need to draw each texture or not
        int activeInputs[2];
        ViewerInstance* internalViewer = _imp->viewerTab->getInternalNode();
        if (!internalViewer) {
            return;
        }
        internalViewer->getActiveInputs(activeInputs[0], activeInputs[1]);
        bool drawTexture[2];
        drawTexture[0] = _imp->activeTextures[0];
        drawTexture[1] = _imp->activeTextures[1] && compOp != eViewerCompositingOperatorNone;
        if ( (activeInputs[0] == activeInputs[1]) && (compOp != eViewerCompositingOperatorMinus) ) {
            drawTexture[1] = false;
        }
        double wipeMix;
        {
            QMutexLocker l(&_imp->wipeControlsMutex);
            wipeMix = _imp->mixAmount;
        }

        GLuint savedTexture;
        glGetIntegerv(GL_TEXTURE_BINDING_2D, (GLint*)&savedTexture);
        {
            GLProtectAttrib a(GL_COLOR_BUFFER_BIT | GL_ENABLE_BIT | GL_CURRENT_BIT);

            clearColorBuffer( _imp->clearColor.redF(),_imp->clearColor.greenF(),_imp->clearColor.blueF(),_imp->clearColor.alphaF() );
            glCheckErrorIgnoreOSXBug();

            glEnable (GL_TEXTURE_2D);
            glColor4d(1., 1., 1., 1.);
            glBlendColor(1, 1, 1, wipeMix);
            
            ///Depending on the premultiplication of the input image we use a different blending func
            ImagePremultiplicationEnum premultA = _imp->displayingImagePremult[0];
            if (!_imp->viewerTab->isCheckerboardEnabled()) {
                premultA = Natron::eImagePremultiplicationOpaque; ///When no checkerboard, draw opaque
            }

            if (compOp == eViewerCompositingOperatorWipe) {
                ///In wipe mode draw first the input A then only the portion we are interested in the input B

                if (drawTexture[0]) {
                    BlendSetter b(premultA);
                    drawRenderingVAO(_imp->displayingImageMipMapLevel[0], 0, eDrawPolygonModeWhole);
                }
                if (drawTexture[1]) {
                    glEnable(GL_BLEND);
                    glBlendFunc(GL_CONSTANT_ALPHA, GL_ONE_MINUS_CONSTANT_ALPHA);
                    drawRenderingVAO(_imp->displayingImageMipMapLevel[1], 1, eDrawPolygonModeWipeRight);
                    glDisable(GL_BLEND);
                }
            } else if (compOp == eViewerCompositingOperatorMinus) {
                if (drawTexture[0]) {
                    BlendSetter b(premultA);
                    drawRenderingVAO(_imp->displayingImageMipMapLevel[0], 0, eDrawPolygonModeWhole);
                }
                if (drawTexture[1]) {
                    glEnable(GL_BLEND);
                    glBlendFunc(GL_CONSTANT_ALPHA, GL_ONE);
                    glBlendEquation(GL_FUNC_REVERSE_SUBTRACT);
                    drawRenderingVAO(_imp->displayingImageMipMapLevel[1], 1, eDrawPolygonModeWipeRight);
                    glDisable(GL_BLEND);
                }
            } else if (compOp == eViewerCompositingOperatorUnder) {
                if (drawTexture[0]) {
                    BlendSetter b(premultA);
                    drawRenderingVAO(_imp->displayingImageMipMapLevel[0], 0, eDrawPolygonModeWhole);
                }
                if (drawTexture[1]) {
                    glEnable(GL_BLEND);
                    glBlendFunc(GL_ONE_MINUS_DST_ALPHA, GL_ONE);
                    drawRenderingVAO(_imp->displayingImageMipMapLevel[1], 1, eDrawPolygonModeWipeRight);
                    glDisable(GL_BLEND);

                    glEnable(GL_BLEND);
                    glBlendFunc(GL_CONSTANT_ALPHA,GL_ONE_MINUS_CONSTANT_ALPHA);
                    drawRenderingVAO(_imp->displayingImageMipMapLevel[1], 1, eDrawPolygonModeWipeRight);
                    glDisable(GL_BLEND);
                }
            } else if (compOp == eViewerCompositingOperatorOver) {
                ///draw first B then A
                if (drawTexture[1]) {
                    ///Depending on the premultiplication of the input image we use a different blending func
                    ImagePremultiplicationEnum premultB = _imp->displayingImagePremult[1];
                    if (!_imp->viewerTab->isCheckerboardEnabled()) {
                        premultB = Natron::eImagePremultiplicationOpaque; ///When no checkerboard, draw opaque
                    }
                    BlendSetter b(premultB);
                    drawRenderingVAO(_imp->displayingImageMipMapLevel[1], 1, eDrawPolygonModeWipeRight);
                }
                if (drawTexture[0]) {
                    glEnable(GL_BLEND);
                    glBlendFunc(GL_ONE, GL_ONE_MINUS_SRC_ALPHA);
                    drawRenderingVAO(_imp->displayingImageMipMapLevel[0], 0, eDrawPolygonModeWipeRight);
                    glDisable(GL_BLEND);

                    drawRenderingVAO(_imp->displayingImageMipMapLevel[0], 0, eDrawPolygonModeWipeLeft);

                    glEnable(GL_BLEND);
                    glBlendFunc(GL_ONE_MINUS_CONSTANT_ALPHA,GL_CONSTANT_ALPHA);
                    drawRenderingVAO(_imp->displayingImageMipMapLevel[0], 0, eDrawPolygonModeWipeRight);
                    glDisable(GL_BLEND);
                }
            } else {
                if (drawTexture[0]) {
                    glDisable(GL_BLEND);
                    BlendSetter b(premultA);
                    drawRenderingVAO(_imp->displayingImageMipMapLevel[0], 0, eDrawPolygonModeWhole);

                }
            }
        } // GLProtectAttrib a(GL_COLOR_BUFFER_BIT | GL_ENABLE_BIT | GL_CURRENT_BIT);

        ///Unbind render textures for overlays
        glBindTexture(GL_TEXTURE_2D, savedTexture);
        
        glCheckError();
        if (_imp->overlay) {
            drawOverlay(getCurrentRenderScale());
        } else {
            const QFont& f = font();
            QFontMetrics fm(f);
            QPointF pos;
            {
                QMutexLocker k(&_imp->zoomCtxMutex);
                pos = _imp->zoomCtx.toZoomCoordinates(10, height() - fm.height());
            }
            renderText(pos.x(), pos.y(), tr("Overlays off"), QColor(200,0,0), f);
        }
        
        if (_imp->ms == eMouseStateSelecting) {
            _imp->drawSelectionRectangle();
        }
        glCheckErrorAssert();
    } // GLProtectAttrib a(GL_TRANSFORM_BIT);
} // paintGL

void
ViewerGL::clearColorBuffer(double r,
                           double g,
                           double b,
                           double a )
{
    // always running in the main thread
    assert( qApp && qApp->thread() == QThread::currentThread() );
    assert( QGLContext::currentContext() == context() );
    {
        GLProtectAttrib att(GL_CURRENT_BIT | GL_COLOR_BUFFER_BIT);

        glClearColor(r,g,b,a);
        glClear(GL_COLOR_BUFFER_BIT);
    } // GLProtectAttrib a(GL_CURRENT_BIT | GL_COLOR_BUFFER_BIT);
}

void
ViewerGL::toggleOverlays()
{
    // always running in the main thread
    assert( qApp && qApp->thread() == QThread::currentThread() );
    _imp->overlay = !_imp->overlay;
    updateGL();
}

void
ViewerGL::toggleWipe()
{
    if (getViewerTab()->getCompositingOperator() != Natron::eViewerCompositingOperatorNone) {
        getViewerTab()->setCompositingOperator(Natron::eViewerCompositingOperatorNone);
    } else {
        getViewerTab()->setCompositingOperator(Natron::eViewerCompositingOperatorWipe);
    }
}


void
ViewerGL::drawOverlay(unsigned int mipMapLevel)
{
    // always running in the main thread
    assert( qApp && qApp->thread() == QThread::currentThread() );
    assert( QGLContext::currentContext() == context() );

    glCheckError();
    
    RectD projectFormatCanonical;
    _imp->getProjectFormatCanonical(projectFormatCanonical);
    renderText(projectFormatCanonical.right(),projectFormatCanonical.bottom(), _imp->currentViewerInfo_resolutionOverlay,_imp->textRenderingColor,*_imp->textFont);


    QPoint topRight( projectFormatCanonical.right(),projectFormatCanonical.top() );
    QPoint topLeft( projectFormatCanonical.left(),projectFormatCanonical.top() );
    QPoint btmLeft( projectFormatCanonical.left(),projectFormatCanonical.bottom() );
    QPoint btmRight( projectFormatCanonical.right(),projectFormatCanonical.bottom() );

    {
        GLProtectAttrib a(GL_COLOR_BUFFER_BIT | GL_LINE_BIT | GL_CURRENT_BIT | GL_ENABLE_BIT);

        glDisable(GL_BLEND);

        glBegin(GL_LINES);

        glColor4f( _imp->displayWindowOverlayColor.redF(),
                  _imp->displayWindowOverlayColor.greenF(),
                  _imp->displayWindowOverlayColor.blueF(),
                  _imp->displayWindowOverlayColor.alphaF() );
        glVertex3f(btmRight.x(),btmRight.y(),1);
        glVertex3f(btmLeft.x(),btmLeft.y(),1);

        glVertex3f(btmLeft.x(),btmLeft.y(),1);
        glVertex3f(topLeft.x(),topLeft.y(),1);

        glVertex3f(topLeft.x(),topLeft.y(),1);
        glVertex3f(topRight.x(),topRight.y(),1);

        glVertex3f(topRight.x(),topRight.y(),1);
        glVertex3f(btmRight.x(),btmRight.y(),1);

        glEnd();
        glCheckErrorIgnoreOSXBug();

        int activeInputs[2];
        getInternalNode()->getActiveInputs(activeInputs[0], activeInputs[1]);
        for (int i = 0; i < 2; ++i) {
            
            if (!_imp->activeTextures[i] || activeInputs[i] == -1) {
                continue;
            }
            if (i == 1 && (_imp->viewerTab->getCompositingOperator() == eViewerCompositingOperatorNone)) {
                break;
            }
            RectD dataW = getRoD(i);
            
            if (dataW != projectFormatCanonical) {
                renderText(dataW.right(), dataW.top(),
                           _imp->currentViewerInfo_topRightBBOXoverlay[i], _imp->rodOverlayColor,*_imp->textFont);
                renderText(dataW.left(), dataW.bottom(),
                           _imp->currentViewerInfo_btmLeftBBOXoverlay[i], _imp->rodOverlayColor,*_imp->textFont);


                QPointF topRight2( dataW.right(), dataW.top() );
                QPointF topLeft2( dataW.left(),dataW.top() );
                QPointF btmLeft2( dataW.left(),dataW.bottom() );
                QPointF btmRight2( dataW.right(),dataW.bottom() );
                glLineStipple(2, 0xAAAA);
                glEnable(GL_LINE_STIPPLE);
                glBegin(GL_LINES);
                glColor4f( _imp->rodOverlayColor.redF(),
                          _imp->rodOverlayColor.greenF(),
                          _imp->rodOverlayColor.blueF(),
                          _imp->rodOverlayColor.alphaF() );
                glVertex3f(btmRight2.x(),btmRight2.y(),1);
                glVertex3f(btmLeft2.x(),btmLeft2.y(),1);

                glVertex3f(btmLeft2.x(),btmLeft2.y(),1);
                glVertex3f(topLeft2.x(),topLeft2.y(),1);

                glVertex3f(topLeft2.x(),topLeft2.y(),1);
                glVertex3f(topRight2.x(),topRight2.y(),1);

                glVertex3f(topRight2.x(),topRight2.y(),1);
                glVertex3f(btmRight2.x(),btmRight2.y(),1);
                glEnd();
                glDisable(GL_LINE_STIPPLE);
                glCheckError();
            }
        }

        bool userRoIEnabled;
        {
            QMutexLocker l(&_imp->userRoIMutex);
            userRoIEnabled = _imp->userRoIEnabled;
        }
        if (userRoIEnabled) {
            drawUserRoI();
        }

        ViewerCompositingOperatorEnum compOperator = _imp->viewerTab->getCompositingOperator();
        if (compOperator != eViewerCompositingOperatorNone) {
            drawWipeControl();
        }


        glCheckError();
        glColor4f(1., 1., 1., 1.);
        double scale = 1. / (1 << mipMapLevel);
        _imp->viewerTab->drawOverlays(scale,scale);
        glCheckError();

        if (_imp->pickerState == ePickerStateRectangle) {
            if ( _imp->viewerTab->getGui()->hasPickers() ) {
                drawPickerRectangle();
            }
        } else if (_imp->pickerState == ePickerStatePoint) {
            if ( _imp->viewerTab->getGui()->hasPickers() ) {
                drawPickerPixel();
            }
        }

    } // GLProtectAttrib a(GL_COLOR_BUFFER_BIT | GL_LINE_BIT | GL_CURRENT_BIT | GL_ENABLE_BIT);
    glCheckError();
    
    if (_imp->displayPersistentMessage) {
        drawPersistentMessage();
    }
} // drawOverlay

void
ViewerGL::drawUserRoI()
{
    // always running in the main thread
    assert( qApp && qApp->thread() == QThread::currentThread() );
    
    {
        GLProtectAttrib a(GL_COLOR_BUFFER_BIT | GL_CURRENT_BIT | GL_ENABLE_BIT);

        glDisable(GL_BLEND);

        glColor4f(0.9, 0.9, 0.9, 1.);

        double zoomScreenPixelWidth, zoomScreenPixelHeight;
        {
            QMutexLocker l(&_imp->zoomCtxMutex);
            zoomScreenPixelWidth = _imp->zoomCtx.screenPixelWidth();
            zoomScreenPixelHeight = _imp->zoomCtx.screenPixelHeight();
        }
        RectD userRoI;
        {
            QMutexLocker l(&_imp->userRoIMutex);
            userRoI = _imp->userRoI;
        }

        ///base rect
        glBegin(GL_LINE_LOOP);
        glVertex2f(userRoI.x1, userRoI.y1); //bottom left
        glVertex2f(userRoI.x1, userRoI.y2); //top left
        glVertex2f(userRoI.x2, userRoI.y2); //top right
        glVertex2f(userRoI.x2, userRoI.y1); //bottom right
        glEnd();


        glBegin(GL_LINES);
        ///border ticks
        double borderTickWidth = USER_ROI_BORDER_TICK_SIZE * zoomScreenPixelWidth;
        double borderTickHeight = USER_ROI_BORDER_TICK_SIZE * zoomScreenPixelHeight;
        glVertex2f(userRoI.x1, (userRoI.y1 + userRoI.y2) / 2);
        glVertex2f(userRoI.x1 - borderTickWidth, (userRoI.y1 + userRoI.y2) / 2);

        glVertex2f(userRoI.x2, (userRoI.y1 + userRoI.y2) / 2);
        glVertex2f(userRoI.x2 + borderTickWidth, (userRoI.y1 + userRoI.y2) / 2);

        glVertex2f( (userRoI.x1 +  userRoI.x2) / 2, userRoI.y2 );
        glVertex2f( (userRoI.x1 +  userRoI.x2) / 2, userRoI.y2 + borderTickHeight );

        glVertex2f( (userRoI.x1 +  userRoI.x2) / 2, userRoI.y1 );
        glVertex2f( (userRoI.x1 +  userRoI.x2) / 2, userRoI.y1 - borderTickHeight );

        ///middle cross
        double crossWidth = USER_ROI_CROSS_RADIUS * zoomScreenPixelWidth;
        double crossHeight = USER_ROI_CROSS_RADIUS * zoomScreenPixelHeight;
        glVertex2f( (userRoI.x1 +  userRoI.x2) / 2, (userRoI.y1 + userRoI.y2) / 2 - crossHeight );
        glVertex2f( (userRoI.x1 +  userRoI.x2) / 2, (userRoI.y1 + userRoI.y2) / 2 + crossHeight );

        glVertex2f( (userRoI.x1 +  userRoI.x2) / 2  - crossWidth, (userRoI.y1 + userRoI.y2) / 2 );
        glVertex2f( (userRoI.x1 +  userRoI.x2) / 2  + crossWidth, (userRoI.y1 + userRoI.y2) / 2 );
        glEnd();


        ///draw handles hint for the user
        glBegin(GL_QUADS);

        double rectHalfWidth = (USER_ROI_SELECTION_POINT_SIZE * zoomScreenPixelWidth) / 2.;
        double rectHalfHeight = (USER_ROI_SELECTION_POINT_SIZE * zoomScreenPixelWidth) / 2.;
        //left
        glVertex2f(userRoI.x1 + rectHalfWidth, (userRoI.y1 + userRoI.y2) / 2 - rectHalfHeight);
        glVertex2f(userRoI.x1 + rectHalfWidth, (userRoI.y1 + userRoI.y2) / 2 + rectHalfHeight);
        glVertex2f(userRoI.x1 - rectHalfWidth, (userRoI.y1 + userRoI.y2) / 2 + rectHalfHeight);
        glVertex2f(userRoI.x1 - rectHalfWidth, (userRoI.y1 + userRoI.y2) / 2 - rectHalfHeight);

        //top
        glVertex2f( (userRoI.x1 +  userRoI.x2) / 2 - rectHalfWidth, userRoI.y2 - rectHalfHeight );
        glVertex2f( (userRoI.x1 +  userRoI.x2) / 2 - rectHalfWidth, userRoI.y2 + rectHalfHeight );
        glVertex2f( (userRoI.x1 +  userRoI.x2) / 2 + rectHalfWidth, userRoI.y2 + rectHalfHeight );
        glVertex2f( (userRoI.x1 +  userRoI.x2) / 2 + rectHalfWidth, userRoI.y2 - rectHalfHeight );

        //right
        glVertex2f(userRoI.x2 - rectHalfWidth, (userRoI.y1 + userRoI.y2) / 2 - rectHalfHeight);
        glVertex2f(userRoI.x2 - rectHalfWidth, (userRoI.y1 + userRoI.y2) / 2 + rectHalfHeight);
        glVertex2f(userRoI.x2 + rectHalfWidth, (userRoI.y1 + userRoI.y2) / 2 + rectHalfHeight);
        glVertex2f(userRoI.x2 + rectHalfWidth, (userRoI.y1 + userRoI.y2) / 2 - rectHalfHeight);

        //bottom
        glVertex2f( (userRoI.x1 +  userRoI.x2) / 2 - rectHalfWidth, userRoI.y1 - rectHalfHeight );
        glVertex2f( (userRoI.x1 +  userRoI.x2) / 2 - rectHalfWidth, userRoI.y1 + rectHalfHeight );
        glVertex2f( (userRoI.x1 +  userRoI.x2) / 2 + rectHalfWidth, userRoI.y1 + rectHalfHeight );
        glVertex2f( (userRoI.x1 +  userRoI.x2) / 2 + rectHalfWidth, userRoI.y1 - rectHalfHeight );

        //middle
        glVertex2f( (userRoI.x1 +  userRoI.x2) / 2 - rectHalfWidth, (userRoI.y1 + userRoI.y2) / 2 - rectHalfHeight );
        glVertex2f( (userRoI.x1 +  userRoI.x2) / 2 - rectHalfWidth, (userRoI.y1 + userRoI.y2) / 2 + rectHalfHeight );
        glVertex2f( (userRoI.x1 +  userRoI.x2) / 2 + rectHalfWidth, (userRoI.y1 + userRoI.y2) / 2 + rectHalfHeight );
        glVertex2f( (userRoI.x1 +  userRoI.x2) / 2 + rectHalfWidth, (userRoI.y1 + userRoI.y2) / 2 - rectHalfHeight );


        //top left
        glVertex2f(userRoI.x1 - rectHalfWidth, userRoI.y2 - rectHalfHeight);
        glVertex2f(userRoI.x1 - rectHalfWidth, userRoI.y2 + rectHalfHeight);
        glVertex2f(userRoI.x1 + rectHalfWidth, userRoI.y2 + rectHalfHeight);
        glVertex2f(userRoI.x1 + rectHalfWidth, userRoI.y2 - rectHalfHeight);

        //top right
        glVertex2f(userRoI.x2 - rectHalfWidth, userRoI.y2 - rectHalfHeight);
        glVertex2f(userRoI.x2 - rectHalfWidth, userRoI.y2 + rectHalfHeight);
        glVertex2f(userRoI.x2 + rectHalfWidth, userRoI.y2 + rectHalfHeight);
        glVertex2f(userRoI.x2 + rectHalfWidth, userRoI.y2 - rectHalfHeight);

        //bottom right
        glVertex2f(userRoI.x2 - rectHalfWidth, userRoI.y1 - rectHalfHeight);
        glVertex2f(userRoI.x2 - rectHalfWidth, userRoI.y1 + rectHalfHeight);
        glVertex2f(userRoI.x2 + rectHalfWidth, userRoI.y1 + rectHalfHeight);
        glVertex2f(userRoI.x2 + rectHalfWidth, userRoI.y1 - rectHalfHeight);
        
        
        //bottom left
        glVertex2f(userRoI.x1 - rectHalfWidth, userRoI.y1 - rectHalfHeight);
        glVertex2f(userRoI.x1 - rectHalfWidth, userRoI.y1 + rectHalfHeight);
        glVertex2f(userRoI.x1 + rectHalfWidth, userRoI.y1 + rectHalfHeight);
        glVertex2f(userRoI.x1 + rectHalfWidth, userRoI.y1 - rectHalfHeight);
        
        glEnd();
    } // GLProtectAttrib a(GL_COLOR_BUFFER_BIT | GL_CURRENT_BIT | GL_ENABLE_BIT);
} // drawUserRoI

void
ViewerGL::drawWipeControl()
{
    double wipeAngle;
    QPointF wipeCenter;
    double mixAmount;
    {
        QMutexLocker l(&_imp->wipeControlsMutex);
        wipeAngle = _imp->wipeAngle;
        wipeCenter = _imp->wipeCenter;
        mixAmount = _imp->mixAmount;
    }
    double alphaMix1,alphaMix0,alphaCurMix;
    double mpi8 = M_PI / 8;

    alphaMix1 = wipeAngle + mpi8;
    alphaMix0 = wipeAngle + 3. * mpi8;
    alphaCurMix = mixAmount * (alphaMix1 - alphaMix0) + alphaMix0;
    QPointF mix0Pos,mixPos,mix1Pos;
    
    double mixX,mixY,rotateW,rotateH,rotateOffsetX,rotateOffsetY;
    
    double zoomScreenPixelWidth, zoomScreenPixelHeight;
    {
        QMutexLocker l(&_imp->zoomCtxMutex);
        zoomScreenPixelWidth = _imp->zoomCtx.screenPixelWidth();
        zoomScreenPixelHeight = _imp->zoomCtx.screenPixelHeight();
    }

    mixX = WIPE_MIX_HANDLE_LENGTH * zoomScreenPixelWidth;
    mixY = WIPE_MIX_HANDLE_LENGTH * zoomScreenPixelHeight;
    rotateW = WIPE_ROTATE_HANDLE_LENGTH * zoomScreenPixelWidth;
    rotateH = WIPE_ROTATE_HANDLE_LENGTH * zoomScreenPixelHeight;
    rotateOffsetX = WIPE_ROTATE_OFFSET * zoomScreenPixelWidth;
    rotateOffsetY = WIPE_ROTATE_OFFSET * zoomScreenPixelHeight;


    mixPos.setX(wipeCenter.x() + std::cos(alphaCurMix) * mixX);
    mixPos.setY(wipeCenter.y() + std::sin(alphaCurMix) * mixY);
    mix0Pos.setX(wipeCenter.x() + std::cos(alphaMix0) * mixX);
    mix0Pos.setY(wipeCenter.y() + std::sin(alphaMix0) * mixY);
    mix1Pos.setX(wipeCenter.x() + std::cos(alphaMix1) * mixX);
    mix1Pos.setY(wipeCenter.y() + std::sin(alphaMix1) * mixY);

    QPointF oppositeAxisBottom,oppositeAxisTop,rotateAxisLeft,rotateAxisRight;
    rotateAxisRight.setX( wipeCenter.x() + std::cos(wipeAngle) * (rotateW - rotateOffsetX) );
    rotateAxisRight.setY( wipeCenter.y() + std::sin(wipeAngle) * (rotateH - rotateOffsetY) );
    rotateAxisLeft.setX(wipeCenter.x() - std::cos(wipeAngle) * rotateOffsetX);
    rotateAxisLeft.setY( wipeCenter.y() - (std::sin(wipeAngle) * rotateOffsetY) );

    oppositeAxisTop.setX( wipeCenter.x() + std::cos(wipeAngle + M_PI / 2.) * (rotateW / 2.) );
    oppositeAxisTop.setY( wipeCenter.y() + std::sin(wipeAngle + M_PI / 2.) * (rotateH / 2.) );
    oppositeAxisBottom.setX( wipeCenter.x() - std::cos(wipeAngle + M_PI / 2.) * (rotateW / 2.) );
    oppositeAxisBottom.setY( wipeCenter.y() - std::sin(wipeAngle + M_PI / 2.) * (rotateH / 2.) );

    {
        GLProtectAttrib a(GL_ENABLE_BIT | GL_LINE_BIT | GL_CURRENT_BIT | GL_HINT_BIT | GL_TRANSFORM_BIT | GL_COLOR_BUFFER_BIT);
        //GLProtectMatrix p(GL_PROJECTION); // useless (we do two glTranslate in opposite directions)

        // Draw everything twice
        // l = 0: shadow
        // l = 1: drawing
        double baseColor[3];
        for (int l = 0; l < 2; ++l) {
            
            // shadow (uses GL_PROJECTION)
            glMatrixMode(GL_PROJECTION);
            int direction = (l == 0) ? 1 : -1;
            // translate (1,-1) pixels
            glTranslated(direction * zoomScreenPixelWidth / 256, -direction * zoomScreenPixelHeight / 256, 0);
            glMatrixMode(GL_MODELVIEW); // Modelview should be used on Nuke
            
            if (l == 0) {
                // Draw a shadow for the cross hair
                baseColor[0] = baseColor[1] = baseColor[2] = 0.;
            } else {
                baseColor[0] = baseColor[1] = baseColor[2] = 0.8;
            }

            glEnable(GL_BLEND);
            glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
            glEnable(GL_LINE_SMOOTH);
            glHint(GL_LINE_SMOOTH_HINT, GL_DONT_CARE);
            glLineWidth(1.5);
            glBegin(GL_LINES);
            if ( (_imp->hs == eHoverStateWipeRotateHandle) || (_imp->ms == eMouseStateRotatingWipeHandle) ) {
                glColor4f(0., 1. * l, 0., 1.);
            }
            glColor4f(baseColor[0],baseColor[1],baseColor[2],1.);
            glVertex2d( rotateAxisLeft.x(), rotateAxisLeft.y() );
            glVertex2d( rotateAxisRight.x(), rotateAxisRight.y() );
            glVertex2d( oppositeAxisBottom.x(), oppositeAxisBottom.y() );
            glVertex2d( oppositeAxisTop.x(), oppositeAxisTop.y() );
            glVertex2d( wipeCenter.x(),wipeCenter.y() );
            glVertex2d( mixPos.x(), mixPos.y() );
            glEnd();
            glLineWidth(1.);

            ///if hovering the rotate handle or dragging it show a small bended arrow
            if ( (_imp->hs == eHoverStateWipeRotateHandle) || (_imp->ms == eMouseStateRotatingWipeHandle) ) {
                GLProtectMatrix p(GL_MODELVIEW);

                glColor4f(0., 1. * l, 0., 1.);
                double arrowCenterX = WIPE_ROTATE_HANDLE_LENGTH * zoomScreenPixelWidth / 2;
                ///draw an arrow slightly bended. This is an arc of circle of radius 5 in X, and 10 in Y.
                OfxPointD arrowRadius;
                arrowRadius.x = 5. * zoomScreenPixelWidth;
                arrowRadius.y = 10. * zoomScreenPixelHeight;

                glTranslatef(wipeCenter.x(), wipeCenter.y(), 0.);
                glRotatef(wipeAngle * 180.0 / M_PI,0, 0, 1);
                //  center the oval at x_center, y_center
                glTranslatef (arrowCenterX, 0., 0);
                //  draw the oval using line segments
                glBegin (GL_LINE_STRIP);
                glVertex2f (0, arrowRadius.y);
                glVertex2f (arrowRadius.x, 0.);
                glVertex2f (0, -arrowRadius.y);
                glEnd ();


                glBegin(GL_LINES);
                ///draw the top head
                glVertex2f(0., arrowRadius.y);
                glVertex2f(0., arrowRadius.y -  arrowRadius.x );

                glVertex2f(0., arrowRadius.y);
                glVertex2f(4. * zoomScreenPixelWidth, arrowRadius.y - 3. * zoomScreenPixelHeight); // 5^2 = 3^2+4^2

                ///draw the bottom head
                glVertex2f(0., -arrowRadius.y);
                glVertex2f(0., -arrowRadius.y + 5. * zoomScreenPixelHeight);

                glVertex2f(0., -arrowRadius.y);
                glVertex2f(4. * zoomScreenPixelWidth, -arrowRadius.y + 3. * zoomScreenPixelHeight); // 5^2 = 3^2+4^2

                glEnd();

                glColor4f(baseColor[0],baseColor[1],baseColor[2],1.);
            }

            glPointSize(5.);
            glEnable(GL_POINT_SMOOTH);
            glBegin(GL_POINTS);
            glVertex2d( wipeCenter.x(), wipeCenter.y() );
            if ( ( (_imp->hs == eHoverStateWipeMix) && (_imp->ms != eMouseStateRotatingWipeHandle) ) || (_imp->ms == eMouseStateDraggingWipeMixHandle) ) {
                glColor4f(0., 1. * l, 0., 1.);
            }
            glVertex2d( mixPos.x(), mixPos.y() );
            glEnd();
            glPointSize(1.);
            
            _imp->drawArcOfCircle(wipeCenter, mixX, mixY, wipeAngle + M_PI / 8., wipeAngle + 3. * M_PI / 8.);
      
        }
    } // GLProtectAttrib a(GL_ENABLE_BIT | GL_LINE_BIT | GL_CURRENT_BIT | GL_HINT_BIT | GL_TRANSFORM_BIT | GL_COLOR_BUFFER_BIT);
} // drawWipeControl

void
ViewerGL::Implementation::drawArcOfCircle(const QPointF & center,
                                          double radiusX,
                                          double radiusY,
                                          double startAngle,
                                          double endAngle)
{
    double alpha = startAngle;
    double x,y;

    {
        GLProtectAttrib a(GL_CURRENT_BIT);

        if ( (hs == eHoverStateWipeMix) || (ms == eMouseStateDraggingWipeMixHandle) ) {
            glColor3f(0, 1, 0);
        }
        glBegin(GL_POINTS);
        while (alpha <= endAngle) {
            x = center.x()  + radiusX * std::cos(alpha);
            y = center.y()  + radiusY * std::sin(alpha);
            glVertex2d(x, y);
            alpha += 0.01;
        }
        glEnd();
    } // GLProtectAttrib a(GL_CURRENT_BIT);
}

void
ViewerGL::drawPickerRectangle()
{
    {
        GLProtectAttrib a(GL_CURRENT_BIT);

        glColor3f(0.9, 0.7, 0.);
        QPointF topLeft = _imp->pickerRect.topLeft();
        QPointF btmRight = _imp->pickerRect.bottomRight();
        ///base rect
        glBegin(GL_LINE_LOOP);
        glVertex2f( topLeft.x(), btmRight.y() ); //bottom left
        glVertex2f( topLeft.x(), topLeft.y() ); //top left
        glVertex2f( btmRight.x(), topLeft.y() ); //top right
        glVertex2f( btmRight.x(), btmRight.y() ); //bottom right
        glEnd();
    } // GLProtectAttrib a(GL_CURRENT_BIT);
}

void
ViewerGL::drawPickerPixel()
{
    {
        GLProtectAttrib a(GL_CURRENT_BIT | GL_ENABLE_BIT | GL_POINT_BIT | GL_COLOR_BUFFER_BIT);

        glEnable(GL_BLEND);
        glBlendFunc(GL_SRC_ALPHA,GL_ONE_MINUS_SRC_ALPHA);
        glEnable(GL_POINT_SMOOTH);
        {
            QMutexLocker l(&_imp->zoomCtxMutex);
            glPointSize( 1. * _imp->zoomCtx.factor() );
        }

        QPointF pos = _imp->lastPickerPos;
        unsigned int mipMapLevel = getInternalNode()->getMipMapLevel();

        if (mipMapLevel != 0) {
            pos *= (1 << mipMapLevel);
        }
        glColor3f(0.9, 0.7, 0.);
        glBegin(GL_POINTS);
        glVertex2d( pos.x(),pos.y() );
        glEnd();
    } // GLProtectAttrib a(GL_CURRENT_BIT | GL_ENABLE_BIT | GL_POINT_BIT | GL_COLOR_BUFFER_BIT);
}

namespace {

    
static QStringList explode(const QString& str)
{
    QRegExp rx("(\\ |\\-|\\.|\\/|\\t|\\n)"); //RegEx for ' ' '/' '.' '-' '\t' '\n'

    QStringList ret;
    int startIndex = 0;
    while (true) {
        int index = str.indexOf(rx, startIndex);
        
        if (index == -1) {
            ret.push_back(str.mid(startIndex));
            return ret;
        }
        
        QString word = str.mid(startIndex, index - startIndex);
        
        const QChar& nextChar = str[index];
        
        // Dashes and the likes should stick to the word occuring before it. Whitespace doesn't have to.
        if (nextChar.isSpace()) {
            ret.push_back(word);
            ret.push_back(nextChar);
        } else {
            ret.push_back(word + nextChar);
        }
        
        startIndex = index + 1;
    }
    return ret;
}
    
static QStringList wordWrap(const QFontMetrics& fm,const QString& str, int width)
{
    QStringList words = explode(str);
    
    int curLineLength = 0;

    QStringList stringL;
    QString curString;
    
    for(int i = 0; i < words.size(); ++i) {
        
        QString word = words[i];
        int wordPixels = fm.width(word);
        
        // If adding the new word to the current line would be too long,
        // then put it on a new line (and split it up if it's too long).
        if (curLineLength + wordPixels > width) {
            // Only move down to a new line if we have text on the current line.
            // Avoids situation where wrapped whitespace causes emptylines in text.
            if (curLineLength > 0) {
                if (!curString.isEmpty()) {
                    stringL.push_back(curString);
                    curString.clear();
                }
                //tmp.append('\n');
                curLineLength = 0;
            }
            
            // If the current word is too long to fit on a line even on it's own then
            // split the word up.
            while (wordPixels > width) {
                
                curString.clear();
                curString.append(word.mid(0, width - 1));
                word = word.mid(width - 1);
                
                if (!curString.isEmpty()) {
                    stringL.push_back(curString);
                    curString.clear();
                }
                wordPixels = fm.width(word);
                //tmp.append('\n');
            }
            
            // Remove leading whitespace from the word so the new line starts flush to the left.
            word = word.trimmed();
            
        }
        curString.append(word);
        curLineLength += wordPixels;
    }
    if (!curString.isEmpty()) {
        stringL.push_back(curString);
    }
    return stringL;
}

}

void
ViewerGL::drawPersistentMessage()
{
    // always running in the main thread
    assert( qApp && qApp->thread() == QThread::currentThread() );
    assert( QGLContext::currentContext() == context() );

    QFontMetrics metrics( *_imp->textFont );

    int offset =  10;
    double metricsHeightZoomCoord;
    QPointF topLeft, bottomRight,offsetZoomCoord;
    
    {
        QMutexLocker l(&_imp->zoomCtxMutex);
        topLeft = _imp->zoomCtx.toZoomCoordinates(0,0);
        bottomRight = _imp->zoomCtx.toZoomCoordinates( _imp->zoomCtx.screenWidth(),_imp->persistentMessages.size() * (metrics.height() + offset) );
        offsetZoomCoord = _imp->zoomCtx.toZoomCoordinates(PERSISTENT_MESSAGE_LEFT_OFFSET_PIXELS, offset);
        metricsHeightZoomCoord = topLeft.y() - _imp->zoomCtx.toZoomCoordinates(0, metrics.height()).y();
    }
    offsetZoomCoord.ry() = topLeft.y() - offsetZoomCoord.y();
    QPointF textPos(offsetZoomCoord.x(),  topLeft.y() - (offsetZoomCoord.y() / 2.) - metricsHeightZoomCoord);
    
    {
        GLProtectAttrib a(GL_COLOR_BUFFER_BIT | GL_ENABLE_BIT);

        glDisable(GL_BLEND);

        if (_imp->persistentMessageType == 1) { // error
            glColor4f(0.5,0.,0.,1.);
        } else { // warning
            glColor4f(0.65,0.65,0.,1.);
        }
        glBegin(GL_POLYGON);
        glVertex2f( topLeft.x(),topLeft.y() ); //top left
        glVertex2f( topLeft.x(),bottomRight.y() ); //bottom left
        glVertex2f( bottomRight.x(),bottomRight.y() ); //bottom right
        glVertex2f( bottomRight.x(),topLeft.y() ); //top right
        glEnd();


        for (int j = 0; j < _imp->persistentMessages.size(); ++j) {
            renderText(textPos.x(),textPos.y(), _imp->persistentMessages.at(j),_imp->textRenderingColor,*_imp->textFont);
            textPos.setY(textPos.y() - (metricsHeightZoomCoord + offsetZoomCoord.y()));/*metrics.height() * 2 * zoomScreenPixelHeight*/
        }
        glCheckError();
    } // GLProtectAttrib a(GL_COLOR_BUFFER_BIT | GL_ENABLE_BIT);
} // drawPersistentMessage

void
ViewerGL::Implementation::drawSelectionRectangle()
{
    {
        GLProtectAttrib a(GL_HINT_BIT | GL_ENABLE_BIT | GL_LINE_BIT | GL_COLOR_BUFFER_BIT | GL_CURRENT_BIT);

        glEnable(GL_BLEND);
        glBlendFunc(GL_SRC_ALPHA,GL_ONE_MINUS_SRC_ALPHA);
        glEnable(GL_LINE_SMOOTH);
        glHint(GL_LINE_SMOOTH_HINT,GL_DONT_CARE);

        glColor4f(0.5,0.8,1.,0.4);
        QPointF btmRight = selectionRectangle.bottomRight();
        QPointF topLeft = selectionRectangle.topLeft();

        glBegin(GL_POLYGON);
        glVertex2f( topLeft.x(),btmRight.y() );
        glVertex2f( topLeft.x(),topLeft.y() );
        glVertex2f( btmRight.x(),topLeft.y() );
        glVertex2f( btmRight.x(),btmRight.y() );
        glEnd();


        glLineWidth(1.5);

        glBegin(GL_LINE_LOOP);
        glVertex2f( topLeft.x(),btmRight.y() );
        glVertex2f( topLeft.x(),topLeft.y() );
        glVertex2f( btmRight.x(),topLeft.y() );
        glVertex2f( btmRight.x(),btmRight.y() );
        glEnd();

        glCheckError();
    } // GLProtectAttrib a(GL_HINT_BIT | GL_ENABLE_BIT | GL_LINE_BIT | GL_COLOR_BUFFER_BIT | GL_CURRENT_BIT);
}

void
ViewerGL::Implementation::drawCheckerboardTexture(const RectD& rod)
{
    ///We divide by 2 the tiles count because one texture is 4 tiles actually
    QPointF topLeft,btmRight;
    double screenW,screenH;
    QPointF rodBtmLeft;
    QPointF rodTopRight;
    {
        QMutexLocker l(&zoomCtxMutex);
        topLeft = zoomCtx.toZoomCoordinates(0, 0);
        screenW = zoomCtx.screenWidth();
        screenH = zoomCtx.screenHeight();
        btmRight = zoomCtx.toZoomCoordinates(screenW - 1, screenH - 1);
        rodBtmLeft = zoomCtx.toWidgetCoordinates(rod.x1, rod.y1);
        rodTopRight = zoomCtx.toWidgetCoordinates(rod.x2, rod.y2);
    }
    
    double xTilesCountF = screenW / (checkerboardTileSize * 4); //< 4 because the texture contains 4 tiles
    double yTilesCountF = screenH / (checkerboardTileSize * 4);

    GLuint savedTexture;
    glGetIntegerv(GL_TEXTURE_BINDING_2D, (GLint*)&savedTexture);
    {
        GLProtectAttrib a(GL_SCISSOR_BIT | GL_ENABLE_BIT);

        glEnable(GL_SCISSOR_TEST);
        glScissor(rodBtmLeft.x(), screenH - rodBtmLeft.y(), rodTopRight.x() - rodBtmLeft.x(), rodBtmLeft.y() - rodTopRight.y());

        glEnable(GL_TEXTURE_2D);
        glBindTexture(GL_TEXTURE_2D, checkerboardTextureID);
        glBegin(GL_POLYGON);
        glTexCoord2d(0., 0.); glVertex2d(topLeft.x(),btmRight.y());
        glTexCoord2d(0., yTilesCountF); glVertex2d(topLeft.x(),topLeft.y());
        glTexCoord2d(xTilesCountF, yTilesCountF); glVertex2d(btmRight.x(), topLeft.y());
        glTexCoord2d(xTilesCountF, 0.); glVertex2d(btmRight.x(), btmRight.y());
        glEnd();


        //glDisable(GL_SCISSOR_TEST);
    } // GLProtectAttrib a(GL_SCISSOR_BIT | GL_ENABLE_BIT);
    glBindTexture(GL_TEXTURE_2D, savedTexture);
    glCheckError();
}

void
ViewerGL::initializeGL()
{
    // always running in the main thread
    assert( qApp && qApp->thread() == QThread::currentThread() );
    makeCurrent();
    initAndCheckGlExtensions();
    _imp->displayTextures[0] = new Texture(GL_TEXTURE_2D, GL_LINEAR, GL_NEAREST, GL_CLAMP_TO_EDGE);
    _imp->displayTextures[1] = new Texture(GL_TEXTURE_2D, GL_LINEAR, GL_NEAREST, GL_CLAMP_TO_EDGE);


    // glGenVertexArrays(1, &_vaoId);
    glGenBuffers(1, &_imp->vboVerticesId);
    glGenBuffers(1, &_imp->vboTexturesId);
    glGenBuffers(1, &_imp->iboTriangleStripId);

    glBindBuffer(GL_ARRAY_BUFFER, _imp->vboTexturesId);
    glBufferData(GL_ARRAY_BUFFER, 32 * sizeof(GLfloat), 0, GL_DYNAMIC_DRAW);

    glBindBuffer(GL_ARRAY_BUFFER, _imp->vboVerticesId);
    glBufferData(GL_ARRAY_BUFFER, 32 * sizeof(GLfloat), 0, GL_DYNAMIC_DRAW);
    glBindBuffer(GL_ARRAY_BUFFER, 0);

    glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, _imp->iboTriangleStripId);
    glBufferData(GL_ELEMENT_ARRAY_BUFFER, 28 * sizeof(GLubyte), triangleStrip, GL_STATIC_DRAW);

    glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, 0);
    glCheckError();

    _imp->initializeCheckerboardTexture(true);
    
    if (_imp->supportsGLSL) {
        initShaderGLSL();
        glCheckError();
    }

    glCheckError();
}

void
ViewerGL::Implementation::initializeCheckerboardTexture(bool mustCreateTexture)
{
    if (mustCreateTexture) {
        glGenTextures(1, &checkerboardTextureID);
    }
    GLuint savedTexture;
    glGetIntegerv(GL_TEXTURE_BINDING_2D, (GLint*)&savedTexture);
    {
        GLProtectAttrib a(GL_ENABLE_BIT);

        glEnable(GL_TEXTURE_2D);
        glBindTexture (GL_TEXTURE_2D,checkerboardTextureID);

        glTexParameteri (GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
        glTexParameteri (GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);

        glTexParameteri (GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_REPEAT);
        glTexParameteri (GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_REPEAT);

        double color1[4];
        double color2[4];
        appPTR->getCurrentSettings()->getCheckerboardColor1(&color1[0], &color1[1], &color1[2], &color1[3]);
        appPTR->getCurrentSettings()->getCheckerboardColor2(&color2[0], &color2[1], &color2[2], &color2[3]);

        unsigned char checkerboardTexture[16];
        ///Fill the first line
        for (int i = 0; i < 4; ++i) {
            checkerboardTexture[i] = Color::floatToInt<256>(color1[i]);
            checkerboardTexture[i + 4] = Color::floatToInt<256>(color2[i]);
        }
        ///Copy the first line to the second line
        memcpy(&checkerboardTexture[8], &checkerboardTexture[4], sizeof(unsigned char) * 4);
        memcpy(&checkerboardTexture[12], &checkerboardTexture[0], sizeof(unsigned char) * 4);

        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, 2, 2, 0, GL_RGBA, GL_UNSIGNED_INT_8_8_8_8_REV, (void*)checkerboardTexture);
    } // GLProtectAttrib a(GL_ENABLE_BIT);
    glBindTexture(GL_TEXTURE_2D, savedTexture);
    
    checkerboardTileSize = appPTR->getCurrentSettings()->getCheckerboardTileSize();

    
   
}

QString
ViewerGL::getOpenGLVersionString() const
{
    // always running in the main thread
    assert( qApp && qApp->thread() == QThread::currentThread() );
    const char* str = (const char*)glGetString(GL_VERSION);
    QString ret;
    if (str) {
        ret.append(str);
    }

    return ret;
}

QString
ViewerGL::getGlewVersionString() const
{
    // always running in the main thread
    assert( qApp && qApp->thread() == QThread::currentThread() );
    const char* str = reinterpret_cast<const char *>( glewGetString(GLEW_VERSION) );
    QString ret;
    if (str) {
        ret.append(str);
    }

    return ret;
}

GLuint
ViewerGL::getPboID(int index)
{
    // always running in the main thread
    assert( qApp && qApp->thread() == QThread::currentThread() );
    assert( QGLContext::currentContext() == context() );

    if ( index >= (int)_imp->pboIds.size() ) {
        GLuint handle;
        glGenBuffers(1,&handle);
        _imp->pboIds.push_back(handle);

        return handle;
    } else {
        return _imp->pboIds[index];
    }
}

/**
 *@returns Returns the current zoom factor that is applied to the display.
 **/
double
ViewerGL::getZoomFactor() const
{
    // MT-SAFE
    QMutexLocker l(&_imp->zoomCtxMutex);

    return _imp->zoomCtx.factor();
}

///imageRoD is in PIXEL COORDINATES
RectI
ViewerGL::getImageRectangleDisplayed(const RectI & imageRoDPixel, // in pixel coordinates
                                     const double par,
                                     unsigned int mipMapLevel)
{
    // MT-SAFE
    RectD visibleArea;
    RectI ret;
    {
        QMutexLocker l(&_imp->zoomCtxMutex);
        QPointF topLeft =  _imp->zoomCtx.toZoomCoordinates(0, 0);
        visibleArea.x1 =  topLeft.x();
        visibleArea.y2 =  topLeft.y();
        QPointF bottomRight = _imp->zoomCtx.toZoomCoordinates(width() - 1, height() - 1);
        visibleArea.x2 = bottomRight.x() ;
        visibleArea.y1 = bottomRight.y();
    }

    if (mipMapLevel != 0) {
        // for the viewer, we need the smallest enclosing rectangle at the mipmap level, in order to avoid black borders
        visibleArea.toPixelEnclosing(mipMapLevel,par,&ret);
    } else {
        ret.x1 = std::floor(visibleArea.x1 / par);
        ret.x2 = std::ceil(visibleArea.x2 / par);
        ret.y1 = std::floor(visibleArea.y1);
        ret.y2 = std::ceil(visibleArea.y2);
    }

    ///If the roi doesn't intersect the image's Region of Definition just return an empty rectangle
    if ( !ret.intersect(imageRoDPixel, &ret) ) {
        ret.clear();
    }

    ///to clip against the user roi however clip it against the mipmaplevel of the zoomFactor+proxy

    RectD userRoI;
    bool userRoiEnabled;
    {
        QMutexLocker l(&_imp->userRoIMutex);
        userRoiEnabled = _imp->userRoIEnabled;
        userRoI = _imp->userRoI;
    }
    if (userRoiEnabled) {
        RectI userRoIpixel;

        ///If the user roi is enabled, we want to render the smallest enclosing rectangle in order to avoid black borders.
        userRoI.toPixelEnclosing(mipMapLevel,par, &userRoIpixel);

        ///If the user roi doesn't intersect the actually visible portion on the viewer, return an empty rectangle.
        if ( !ret.intersect(userRoIpixel, &ret) ) {
            ret.clear();
        }
    }

    return ret;
}

RectI
ViewerGL::getExactImageRectangleDisplayed(const RectD & rod,const double par,unsigned int mipMapLevel)
{
    bool clipToProject = isClippingImageToProjectWindow();
    RectD clippedRod;
    
    if (clipToProject) {
        RectD projectFormatCanonical;
        _imp->getProjectFormatCanonical(projectFormatCanonical);
        rod.intersect(projectFormatCanonical,&clippedRod);
    } else {
        clippedRod = rod;
    }
    
    RectI bounds;
    clippedRod.toPixelEnclosing(mipMapLevel, par, &bounds);
    RectI roi = getImageRectangleDisplayed(bounds, par, mipMapLevel);
    return roi;
}

RectI
ViewerGL::getImageRectangleDisplayedRoundedToTileSize(const RectD & rod,const double par,unsigned int mipMapLevel)
{
    bool clipToProject = isClippingImageToProjectWindow();
    RectD clippedRod;
    
    if (clipToProject) {
        RectD projectFormatCanonical;
        _imp->getProjectFormatCanonical(projectFormatCanonical);
        rod.intersect(projectFormatCanonical,&clippedRod);
    } else {
        clippedRod = rod;
    }
    
    RectI bounds;
    clippedRod.toPixelEnclosing(mipMapLevel, par, &bounds);
    RectI roi = getImageRectangleDisplayed(bounds, par, mipMapLevel);
    
    ////Texrect is the coordinates of the 4 corners of the texture in the bounds with the current zoom
    ////factor taken into account.
    RectI texRect;
    double tileSize = std::pow( 2., (double)appPTR->getCurrentSettings()->getViewerTilesPowerOf2() );
    texRect.x1 = std::floor( ( (double)roi.x1 ) / tileSize ) * tileSize;
    texRect.y1 = std::floor( ( (double)roi.y1 ) / tileSize ) * tileSize;
    texRect.x2 = std::ceil( ( (double)roi.x2 ) / tileSize ) * tileSize;
    texRect.y2 = std::ceil( ( (double)roi.y2 ) / tileSize ) * tileSize;
    
    ///Make sure the bounds of the area to render in the texture lies in the bounds
    texRect.intersect(bounds, &texRect);

    return texRect;
}

int
ViewerGL::isExtensionSupported(const char *extension)
{
    // always running in the main thread
    assert( qApp && qApp->thread() == QThread::currentThread() );
    const GLubyte *extensions = NULL;
    const GLubyte *start;
    GLubyte *where, *terminator;
    where = (GLubyte *) strchr(extension, ' ');
    if ( where || (*extension == '\0') ) {
        return 0;
    }
    extensions = glGetString(GL_EXTENSIONS);
    start = extensions;
    for (;; ) {
        where = (GLubyte *) strstr( (const char *) start, extension );
        if (!where) {
            break;
        }
        terminator = where + strlen(extension);
        if ( (where == start) || (*(where - 1) == ' ') ) {
            if ( (*terminator == ' ') || (*terminator == '\0') ) {
                return 1;
            }
        }
        start = terminator;
    }

    return 0;
}

void
ViewerGL::initAndCheckGlExtensions()
{
    // always running in the main thread
    assert( qApp && qApp->thread() == QThread::currentThread() );
    assert( QGLContext::currentContext() == context() );
    GLenum err = glewInit();
    if (GLEW_OK != err) {
        /* Problem: glewInit failed, something is seriously wrong. */
        Natron::errorDialog( tr("OpenGL/GLEW error").toStdString(),
                             (const char*)glewGetErrorString(err) );
    }
    //fprintf(stdout, "Status: Using GLEW %s\n", glewGetString(GLEW_VERSION));

    // is GL_VERSION_2_0 necessary? note that GL_VERSION_2_0 includes GLSL
    if ( !glewIsSupported("GL_VERSION_1_5 "
                          "GL_ARB_texture_non_power_of_two " // or GL_IMG_texture_npot, or GL_OES_texture_npot, core since 2.0
                          "GL_ARB_shader_objects " // GLSL, Uniform*, core since 2.0
                          "GL_ARB_vertex_buffer_object " // BindBuffer, MapBuffer, etc.
                          "GL_ARB_pixel_buffer_object " // BindBuffer(PIXEL_UNPACK_BUFFER,...
                          //"GL_ARB_vertex_array_object " // BindVertexArray, DeleteVertexArrays, GenVertexArrays, IsVertexArray (VAO), core since 3.0
                          //"GL_ARB_framebuffer_object " // or GL_EXT_framebuffer_object GenFramebuffers, core since version 3.0
                          ) ) {
        Natron::errorDialog( tr("Missing OpenGL requirements").toStdString(),
                             tr("The viewer may not be fully functional. "
                                "This software needs at least OpenGL 1.5 with NPOT textures, GLSL, VBO, PBO, vertex arrays. ").toStdString() );
    }

    _imp->viewerTab->getGui()->setOpenGLVersion( getOpenGLVersionString() );
    _imp->viewerTab->getGui()->setGlewVersion( getGlewVersionString() );

    if ( !QGLShaderProgram::hasOpenGLShaderPrograms( context() ) ) {
        // no need to pull out a dialog, it was already presented after the GLEW check above

        //Natron::errorDialog("Viewer error","The viewer is unabgile to work without a proper version of GLSL.");
        //cout << "Warning : GLSL not present on this hardware, no material acceleration possible." << endl;
        _imp->supportsGLSL = false;
    }
}

void
ViewerGL::Implementation::activateShaderRGB(int texIndex)
{
    // always running in the main thread
    assert( qApp && qApp->thread() == QThread::currentThread() );

    // we assume that:
    // - 8-bits textures are stored non-linear and must be displayer as is
    // - floating-point textures are linear and must be decompressed according to the given lut

    assert(supportsGLSL);

    if ( !shaderRGB->bind() ) {
        cout << qPrintable( shaderRGB->log() ) << endl;
    }

    shaderRGB->setUniformValue("Tex", 0);
    shaderRGB->setUniformValue("gain", (float)displayingImageGain[texIndex]);
    shaderRGB->setUniformValue("offset", (float)displayingImageOffset[texIndex]);
    shaderRGB->setUniformValue("lut", (GLint)displayingImageLut);
}

void
ViewerGL::initShaderGLSL()
{
    // always running in the main thread
    assert( qApp && qApp->thread() == QThread::currentThread() );
    assert( QGLContext::currentContext() == context() );

    if (!_imp->shaderLoaded && _imp->supportsGLSL) {
        _imp->shaderBlack = new QGLShaderProgram( context() );
        if ( !_imp->shaderBlack->addShaderFromSourceCode(QGLShader::Vertex,vertRGB) ) {
            cout << qPrintable( _imp->shaderBlack->log() ) << endl;
        }
        if ( !_imp->shaderBlack->addShaderFromSourceCode(QGLShader::Fragment,blackFrag) ) {
            cout << qPrintable( _imp->shaderBlack->log() ) << endl;
        }
        if ( !_imp->shaderBlack->link() ) {
            cout << qPrintable( _imp->shaderBlack->log() ) << endl;
        }

        _imp->shaderRGB = new QGLShaderProgram( context() );
        if ( !_imp->shaderRGB->addShaderFromSourceCode(QGLShader::Vertex,vertRGB) ) {
            cout << qPrintable( _imp->shaderRGB->log() ) << endl;
        }
        if ( !_imp->shaderRGB->addShaderFromSourceCode(QGLShader::Fragment,fragRGB) ) {
            cout << qPrintable( _imp->shaderRGB->log() ) << endl;
        }

        if ( !_imp->shaderRGB->link() ) {
            cout << qPrintable( _imp->shaderRGB->log() ) << endl;
        }
        _imp->shaderLoaded = true;
    }
}



void
ViewerGL::transferBufferFromRAMtoGPU(const unsigned char* ramBuffer,
                                     const boost::shared_ptr<Natron::Image>& image,
                                     int time,
                                     const RectD& rod,
                                     size_t bytesCount,
                                     const TextureRect & region,
                                     double gain,
                                     double offset,
                                     int lut,
                                     int pboIndex,
                                     unsigned int mipMapLevel,
                                     Natron::ImagePremultiplicationEnum premult,
                                     int textureIndex)
{
    // always running in the main thread
    assert( qApp && qApp->thread() == QThread::currentThread() );
    assert( QGLContext::currentContext() == context() );
    (void)glGetError();
    GLint currentBoundPBO = 0;
    glGetIntegerv(GL_PIXEL_UNPACK_BUFFER_BINDING, &currentBoundPBO);
    GLenum err = glGetError();
    if ( (err != GL_NO_ERROR) || (currentBoundPBO != 0) ) {
        qDebug() << "(ViewerGL::allocateAndMapPBO): Another PBO is currently mapped, glMap failed." << endl;
    }

    glBindBufferARB( GL_PIXEL_UNPACK_BUFFER_ARB, getPboID(pboIndex) );
    glBufferDataARB(GL_PIXEL_UNPACK_BUFFER_ARB, bytesCount, NULL, GL_DYNAMIC_DRAW_ARB);
    GLvoid *ret = glMapBufferARB(GL_PIXEL_UNPACK_BUFFER_ARB, GL_WRITE_ONLY_ARB);
    glCheckError();
    assert(ret);

    memcpy(ret, (void*)ramBuffer, bytesCount);

    glUnmapBufferARB(GL_PIXEL_UNPACK_BUFFER_ARB);
    glCheckError();

    OpenGLViewerI::BitDepthEnum bd = getBitDepth();
    assert(textureIndex == 0 || textureIndex == 1);
    if (bd == OpenGLViewerI::eBitDepthByte) {
        _imp->displayTextures[textureIndex]->fillOrAllocateTexture(region, Texture::eDataTypeByte);
    } else if ( (bd == OpenGLViewerI::eBitDepthFloat) || (bd == OpenGLViewerI::eBitDepthHalf) ) {
        //do 32bit fp textures either way, don't bother with half float. We might support it further on.
        _imp->displayTextures[textureIndex]->fillOrAllocateTexture(region, Texture::eDataTypeFloat);
    }
    glBindBufferARB(GL_PIXEL_UNPACK_BUFFER_ARB, currentBoundPBO);
    //glBindTexture(GL_TEXTURE_2D, 0); // why should we bind texture 0?
    glCheckError();
    _imp->activeTextures[textureIndex] = _imp->displayTextures[textureIndex];
    _imp->displayingImageGain[textureIndex] = gain;
    _imp->displayingImageOffset[textureIndex] = offset;
    _imp->displayingImageMipMapLevel[textureIndex] = mipMapLevel;
    _imp->displayingImageLut = (Natron::ViewerColorSpaceEnum)lut;
    _imp->displayingImagePremult[textureIndex] = premult;
    _imp->displayingImageTime[textureIndex] = time;
    ViewerInstance* internalNode = getInternalNode();
    
    if (_imp->memoryHeldByLastRenderedImages[textureIndex] > 0) {
        internalNode->unregisterPluginMemory(_imp->memoryHeldByLastRenderedImages[textureIndex]);
        _imp->memoryHeldByLastRenderedImages[textureIndex] = 0;
    }
    


    if (image) {
        _imp->viewerTab->setImageFormat(textureIndex, image->getComponents(), image->getBitDepth());
        RectI pixelRoD;
        image->getRoD().toPixelEnclosing(0, image->getPixelAspectRatio(), &pixelRoD);
        {
            QMutexLocker k(&_imp->projectFormatMutex);
            _imp->currentViewerInfo[textureIndex].setDisplayWindow(Format(_imp->projectFormat, image->getPixelAspectRatio()));
        }
        {
            QMutexLocker k(&_imp->lastRenderedImageMutex);
            _imp->lastRenderedImage[textureIndex][mipMapLevel] = image;
        }
        _imp->memoryHeldByLastRenderedImages[textureIndex] = image->size();
        internalNode->registerPluginMemory(_imp->memoryHeldByLastRenderedImages[textureIndex]);
        emit imageChanged(textureIndex,true);
    } else {
        if (!_imp->lastRenderedImage[textureIndex][mipMapLevel]) {
            emit imageChanged(textureIndex,false);
        }
    }
    setRegionOfDefinition(rod,region.par,textureIndex);

    
}

void
ViewerGL::clearLastRenderedImage()
{
    assert( qApp && qApp->thread() == QThread::currentThread() );
    
    ViewerInstance* internalNode = getInternalNode();

    for (int i = 0; i < 2; ++i) {
        for (U32 j = 0; j < _imp->lastRenderedImage[i].size(); ++j) {
            _imp->lastRenderedImage[i][j].reset();
        }
        if (_imp->memoryHeldByLastRenderedImages[i] > 0) {
            internalNode->unregisterPluginMemory(_imp->memoryHeldByLastRenderedImages[i]);
            _imp->memoryHeldByLastRenderedImages[i] = 0;
        }
    }
}

void
ViewerGL::disconnectInputTexture(int textureIndex)
{
    // always running in the main thread
    assert( qApp && qApp->thread() == QThread::currentThread() );
    assert(textureIndex == 0 || textureIndex == 1);
    if (_imp->activeTextures[textureIndex] != 0) {
        _imp->activeTextures[textureIndex] = 0;
        RectI r(0,0,0,0);
        _imp->infoViewer[textureIndex]->setDataWindow(r);
    }
}

void
ViewerGL::setGain(double d)
{
    // always running in the main thread
    assert( qApp && qApp->thread() == QThread::currentThread() );
    _imp->displayingImageGain[0] = d;
    _imp->displayingImageGain[1] = d;
}

void
ViewerGL::setLut(int lut)
{
    // always running in the main thread
    assert( qApp && qApp->thread() == QThread::currentThread() );
    _imp->displayingImageLut = (Natron::ViewerColorSpaceEnum)lut;
}

/**
 *@returns Returns true if the graphic card supports GLSL.
 **/
bool
ViewerGL::supportsGLSL() const
{
    // always running in the main thread
    assert( qApp && qApp->thread() == QThread::currentThread() );

    return _imp->supportsGLSL;
}

#if QT_VERSION < 0x050000
#define QMouseEventLocalPos(e) ( e->posF() )
#else
#define QMouseEventLocalPos(e) ( e->localPos() )
#endif

void
ViewerGL::mousePressEvent(QMouseEvent* e)
{
    // always running in the main thread
    assert( qApp && qApp->thread() == QThread::currentThread() );
    if ( !_imp->viewerTab->getGui() ) {
        return;
    }
    
    _imp->hasMovedSincePress = false;
    
    ///Set focus on user click
    setFocus();
    
    Qt::KeyboardModifiers modifiers = e->modifiers();
    Qt::MouseButton button = e->button();

    if ( buttonDownIsLeft(e) ) {
        _imp->viewerTab->getGui()->selectNode( _imp->viewerTab->getGui()->getApp()->getNodeGui( _imp->viewerTab->getInternalNode()->getNode() ) );
    }

    _imp->oldClick = e->pos();
    _imp->lastMousePosition = e->pos();
    QPointF zoomPos;
    double zoomScreenPixelWidth, zoomScreenPixelHeight; // screen pixel size in zoom coordinates
    {
        QMutexLocker l(&_imp->zoomCtxMutex);
        zoomPos = _imp->zoomCtx.toZoomCoordinates( e->x(), e->y() );
        zoomScreenPixelWidth = _imp->zoomCtx.screenPixelWidth();
        zoomScreenPixelHeight = _imp->zoomCtx.screenPixelHeight();
    }
    RectD userRoI;
    bool userRoIEnabled;
    {
        QMutexLocker l(&_imp->userRoIMutex);
        userRoI = _imp->userRoI;
        userRoIEnabled = _imp->userRoIEnabled;
    }
    bool overlaysCaught = false;
    bool mustRedraw = false;

    bool hasPickers = _imp->viewerTab->getGui()->hasPickers();

    
    if ( (buttonDownIsMiddle(e) || ( (e)->buttons() == Qt::RightButton   && buttonControlAlt(e) == Qt::AltModifier )) && !modifierHasControl(e) ) {
        // middle (or Alt + left) or Alt + right = pan
        _imp->ms = eMouseStateDraggingImage;
        overlaysCaught = true;
    } else if ((e->buttons() & Qt::MiddleButton) && (buttonControlAlt(e) == Qt::AltModifier || (e->buttons() & Qt::LeftButton)) ) {
        // Alt + middle = zoom or Left + middle = zoom
        _imp->ms = eMouseStateZoomingImage;
        overlaysCaught = true;
    } else if ( hasPickers && isMouseShortcut(kShortcutGroupViewer, kShortcutIDMousePickColor, modifiers, button) && displayingImage() ) {
        // picker with single-point selection
        _imp->pickerState = ePickerStatePoint;
        if ( pickColor( e->x(),e->y() ) ) {
            _imp->ms = eMouseStatePickingColor;
            mustRedraw = true;
            overlaysCaught = true;
        }
    } else if ( (_imp->ms == eMouseStateUndefined) && _imp->overlay ) {
        unsigned int mipMapLevel = getCurrentRenderScale();
        double scale = 1. / (1 << mipMapLevel);
        overlaysCaught = _imp->viewerTab->notifyOverlaysPenDown(scale,scale, QMouseEventLocalPos(e), zoomPos, e);
        if (overlaysCaught) {
            mustRedraw = true;
        }
    }


    if (!overlaysCaught) {

          if ( hasPickers && isMouseShortcut(kShortcutGroupViewer, kShortcutIDMouseRectanglePick, modifiers, button) && displayingImage() ) {
            // start picker with rectangle selection (picked color is the average over the rectangle)
            _imp->pickerState = ePickerStateRectangle;
            _imp->pickerRect.setTopLeft(zoomPos);
            _imp->pickerRect.setBottomRight(zoomPos);
            _imp->ms = eMouseStateBuildingPickerRectangle;
            mustRedraw = true;
            overlaysCaught = true;
        } else if ( (_imp->pickerState != ePickerStateInactive) && buttonDownIsLeft(e) && displayingImage() ) {
            // disable picker if picker is set when clicking
            _imp->pickerState = ePickerStateInactive;
            mustRedraw = true;
            overlaysCaught = true;
        } else if ( buttonDownIsLeft(e) &&
                    userRoIEnabled && isNearByUserRoIBottomEdge(userRoI,zoomPos, zoomScreenPixelWidth, zoomScreenPixelHeight) ) {
            // start dragging the bottom edge of the user ROI
            _imp->ms = eMouseStateDraggingRoiBottomEdge;
            overlaysCaught = true;
        } else if ( buttonDownIsLeft(e) &&
                    userRoIEnabled && isNearByUserRoILeftEdge(userRoI,zoomPos, zoomScreenPixelWidth, zoomScreenPixelHeight) ) {
            // start dragging the left edge of the user ROI
            _imp->ms = eMouseStateDraggingRoiLeftEdge;
            overlaysCaught = true;
        } else if ( buttonDownIsLeft(e) &&
                    userRoIEnabled && isNearByUserRoIRightEdge(userRoI,zoomPos, zoomScreenPixelWidth, zoomScreenPixelHeight) ) {
            // start dragging the right edge of the user ROI
            _imp->ms = eMouseStateDraggingRoiRightEdge;
            overlaysCaught = true;
        } else if ( buttonDownIsLeft(e) &&
                    userRoIEnabled && isNearByUserRoITopEdge(userRoI,zoomPos, zoomScreenPixelWidth, zoomScreenPixelHeight) ) {
            // start dragging the top edge of the user ROI
            _imp->ms = eMouseStateDraggingRoiTopEdge;
            overlaysCaught = true;
        } else if ( buttonDownIsLeft(e) &&
                    userRoIEnabled && isNearByUserRoI( (userRoI.x1 + userRoI.x2) / 2., (userRoI.y1 + userRoI.y2) / 2.,
                                     zoomPos, zoomScreenPixelWidth, zoomScreenPixelHeight ) ) {
            // start dragging the midpoint of the user ROI
            _imp->ms = eMouseStateDraggingRoiCross;
            overlaysCaught = true;
        } else if ( buttonDownIsLeft(e) &&
                    userRoIEnabled && isNearByUserRoI(userRoI.x1, userRoI.y2, zoomPos, zoomScreenPixelWidth, zoomScreenPixelHeight) ) {
            // start dragging the topleft corner of the user ROI
            _imp->ms = eMouseStateDraggingRoiTopLeft;
            overlaysCaught = true;
        } else if ( buttonDownIsLeft(e) &&
                    userRoIEnabled && isNearByUserRoI(userRoI.x2, userRoI.y2, zoomPos, zoomScreenPixelWidth, zoomScreenPixelHeight) ) {
            // start dragging the topright corner of the user ROI
            _imp->ms = eMouseStateDraggingRoiTopRight;
            overlaysCaught = true;
        }  else if ( buttonDownIsLeft(e) &&
                     userRoIEnabled && isNearByUserRoI(userRoI.x1, userRoI.y1, zoomPos, zoomScreenPixelWidth, zoomScreenPixelHeight) ) {
            // start dragging the bottomleft corner of the user ROI
            _imp->ms = eMouseStateDraggingRoiBottomLeft;
            overlaysCaught = true;
        }  else if ( buttonDownIsLeft(e) &&
                     userRoIEnabled && isNearByUserRoI(userRoI.x2, userRoI.y1, zoomPos, zoomScreenPixelWidth, zoomScreenPixelHeight) ) {
            // start dragging the bottomright corner of the user ROI
            _imp->ms = eMouseStateDraggingRoiBottomRight;
            overlaysCaught = true;
        } else if ( _imp->overlay && isWipeHandleVisible() &&
                    buttonDownIsLeft(e) && _imp->isNearbyWipeCenter(zoomPos, zoomScreenPixelWidth, zoomScreenPixelHeight) ) {
            _imp->ms = eMouseStateDraggingWipeCenter;
            overlaysCaught = true;
        } else if ( _imp->overlay &&  isWipeHandleVisible() &&
                    buttonDownIsLeft(e) && _imp->isNearbyWipeMixHandle(zoomPos, zoomScreenPixelWidth, zoomScreenPixelHeight) ) {
            _imp->ms = eMouseStateDraggingWipeMixHandle;
            overlaysCaught = true;
        } else if ( _imp->overlay &&  isWipeHandleVisible() &&
                    buttonDownIsLeft(e) && _imp->isNearbyWipeRotateBar(zoomPos, zoomScreenPixelWidth ,zoomScreenPixelHeight) ) {
            _imp->ms = eMouseStateRotatingWipeHandle;
            overlaysCaught = true;
        }
    }

    if (!overlaysCaught) {
        if ( buttonDownIsRight(e) ) {
            _imp->menu->exec( mapToGlobal( e->pos() ) );
        } else if ( buttonDownIsLeft(e) ) {
            ///build selection rectangle
            _imp->selectionRectangle.setTopLeft(zoomPos);
            _imp->selectionRectangle.setBottomRight(zoomPos);
            _imp->lastDragStartPos = zoomPos;
            _imp->ms = eMouseStateSelecting;
            if ( !modCASIsControl(e) ) {
                emit selectionCleared();
                mustRedraw = true;
            }
        }
    }

    if (mustRedraw) {
        updateGL();
    }
} // mousePressEvent

void
ViewerGL::mouseReleaseEvent(QMouseEvent* e)
{
    // always running in the main thread
    assert( qApp && qApp->thread() == QThread::currentThread() );

    if (!_imp->viewerTab->getGui()) {
        return;
    }
    
    
    bool mustRedraw = false;
    if (_imp->ms == eMouseStateBuildingPickerRectangle) {
        updateRectangleColorPicker();
    }

    if (_imp->ms == eMouseStateSelecting) {
        mustRedraw = true;
        if (_imp->hasMovedSincePress) {
            emit selectionRectangleChanged(true);
        }
    }
    
    _imp->hasMovedSincePress = false;


    _imp->ms = eMouseStateUndefined;
    QPointF zoomPos;
    {
        QMutexLocker l(&_imp->zoomCtxMutex);
        zoomPos = _imp->zoomCtx.toZoomCoordinates( e->x(), e->y() );
    }
    unsigned int mipMapLevel = getCurrentRenderScale();
    double scale = 1. / (1 << mipMapLevel);
    if ( _imp->viewerTab->notifyOverlaysPenUp(scale,scale, QMouseEventLocalPos(e), zoomPos, e) ) {
        mustRedraw = true;
    }
    if (mustRedraw) {
        updateGL();
    }
}

void
ViewerGL::mouseMoveEvent(QMouseEvent* e)
{
    // always running in the main thread
    assert( qApp && qApp->thread() == QThread::currentThread() );

    ///The app is closing don't do anything
    if ( !_imp->viewerTab->getGui() || !getInternalNode()) {
        QGLWidget::mouseMoveEvent(e);
        return;
    }

    _imp->hasMovedSincePress = true;
    
    QPointF zoomPos;

    // if the picker was deselected, this fixes the picer State
    // (see issue #133 https://github.com/MrKepzie/Natron/issues/133 )
    if ( !_imp->viewerTab->getGui()->hasPickers() ) {
        _imp->pickerState = ePickerStateInactive;
    }

    double zoomScreenPixelWidth, zoomScreenPixelHeight; // screen pixel size in zoom coordinates
    {
        QMutexLocker l(&_imp->zoomCtxMutex);
        zoomPos = _imp->zoomCtx.toZoomCoordinates( e->x(), e->y() );
        zoomScreenPixelWidth = _imp->zoomCtx.screenPixelWidth();
        zoomScreenPixelHeight = _imp->zoomCtx.screenPixelHeight();
    }
    Format dispW = getDisplayWindow();
    RectD canonicalDispW = dispW.toCanonicalFormat();
    for (int i = 0; i < 2; ++i) {
        const RectD& rod = getRoD(i);
        updateInfoWidgetColorPicker(zoomPos, e->pos(), width(), height(), rod, canonicalDispW, i);
    }
    
    //update the cursor if it is hovering an overlay and we're not dragging the image
    bool userRoIEnabled;
    RectD userRoI;
    {
        QMutexLocker l(&_imp->userRoIMutex);
        userRoI = _imp->userRoI;
        userRoIEnabled = _imp->userRoIEnabled;
    }
    bool mustRedraw = false;
    bool wasHovering = _imp->hs != eHoverStateNothing;

    if ( (_imp->ms == eMouseStateDraggingImage) || !_imp->overlay ) {
        unsetCursor();

    } else {
        _imp->hs = eHoverStateNothing;
        if ( isWipeHandleVisible() && _imp->isNearbyWipeCenter(zoomPos, zoomScreenPixelWidth, zoomScreenPixelHeight) ) {
            setCursor( QCursor(Qt::SizeAllCursor) );
        } else if ( isWipeHandleVisible() && _imp->isNearbyWipeMixHandle(zoomPos, zoomScreenPixelWidth, zoomScreenPixelHeight) ) {
            _imp->hs = eHoverStateWipeMix;
            mustRedraw = true;
        } else if ( isWipeHandleVisible() && _imp->isNearbyWipeRotateBar(zoomPos, zoomScreenPixelWidth, zoomScreenPixelHeight) ) {
            _imp->hs = eHoverStateWipeRotateHandle;
            mustRedraw = true;
        } else if (userRoIEnabled) {
            if ( isNearByUserRoIBottomEdge(userRoI,zoomPos, zoomScreenPixelWidth, zoomScreenPixelHeight)
                 || isNearByUserRoITopEdge(userRoI,zoomPos, zoomScreenPixelWidth, zoomScreenPixelHeight)
                 || ( _imp->ms == eMouseStateDraggingRoiBottomEdge)
                 || ( _imp->ms == eMouseStateDraggingRoiTopEdge) ) {
                setCursor( QCursor(Qt::SizeVerCursor) );
            } else if ( isNearByUserRoILeftEdge(userRoI,zoomPos, zoomScreenPixelWidth, zoomScreenPixelHeight)
                        || isNearByUserRoIRightEdge(userRoI,zoomPos, zoomScreenPixelWidth, zoomScreenPixelHeight)
                        || ( _imp->ms == eMouseStateDraggingRoiLeftEdge)
                        || ( _imp->ms == eMouseStateDraggingRoiRightEdge) ) {
                setCursor( QCursor(Qt::SizeHorCursor) );
            } else if ( isNearByUserRoI( (userRoI.x1 + userRoI.x2) / 2, (userRoI.y1 + userRoI.y2) / 2, zoomPos, zoomScreenPixelWidth, zoomScreenPixelHeight )
                        || ( _imp->ms == eMouseStateDraggingRoiCross) ) {
                setCursor( QCursor(Qt::SizeAllCursor) );
            } else if ( isNearByUserRoI(userRoI.x2, userRoI.y1, zoomPos, zoomScreenPixelWidth, zoomScreenPixelHeight) ||
                        isNearByUserRoI(userRoI.x1, userRoI.y2, zoomPos, zoomScreenPixelWidth, zoomScreenPixelHeight) ||
                        ( _imp->ms == eMouseStateDraggingRoiBottomRight) ||
                        ( _imp->ms == eMouseStateDraggingRoiTopLeft) ) {
                setCursor( QCursor(Qt::SizeFDiagCursor) );
            } else if ( isNearByUserRoI(userRoI.x1, userRoI.y1,zoomPos, zoomScreenPixelWidth, zoomScreenPixelHeight) ||
                        isNearByUserRoI(userRoI.x2, userRoI.y2,zoomPos, zoomScreenPixelWidth, zoomScreenPixelHeight) ||
                        ( _imp->ms == eMouseStateDraggingRoiBottomLeft) ||
                        ( _imp->ms == eMouseStateDraggingRoiTopRight) ) {
                setCursor( QCursor(Qt::SizeBDiagCursor) );
            } else {
                unsetCursor();
            }
        } else {
            unsetCursor();
        }
    }

    if ( (_imp->hs == eHoverStateNothing) && wasHovering ) {
        mustRedraw = true;
    }

    QPoint newClick = e->pos();
    QPoint oldClick = _imp->oldClick;
    QPointF newClick_opengl, oldClick_opengl, oldPosition_opengl;
    {
        QMutexLocker l(&_imp->zoomCtxMutex);
        newClick_opengl = _imp->zoomCtx.toZoomCoordinates( newClick.x(),newClick.y() );
        oldClick_opengl = _imp->zoomCtx.toZoomCoordinates( oldClick.x(),oldClick.y() );
        oldPosition_opengl = _imp->zoomCtx.toZoomCoordinates( _imp->lastMousePosition.x(), _imp->lastMousePosition.y() );
    }
    double dx = ( oldClick_opengl.x() - newClick_opengl.x() );
    double dy = ( oldClick_opengl.y() - newClick_opengl.y() );
    double dxSinceLastMove = ( oldPosition_opengl.x() - newClick_opengl.x() );
    double dySinceLastMove = ( oldPosition_opengl.y() - newClick_opengl.y() );

    switch (_imp->ms) {
    case eMouseStateDraggingImage: {
        {
            QMutexLocker l(&_imp->zoomCtxMutex);
            _imp->zoomCtx.translate(dx, dy);
            _imp->zoomOrPannedSinceLastFit = true;
        }
        _imp->oldClick = newClick;
        _imp->viewerTab->getInternalNode()->renderCurrentFrame(false);
        
        //  else {
        mustRedraw = true;
        // }
        // no need to update the color picker or mouse posn: they should be unchanged
        break;
    }
    case eMouseStateZoomingImage: {
        const double zoomFactor_min = 0.01;
        const double zoomFactor_max = 1024.;
        double zoomFactor;
        int delta = 2*((e->x() - _imp->lastMousePosition.x()) - (e->y() - _imp->lastMousePosition.y()));
        double scaleFactor = std::pow(NATRON_WHEEL_ZOOM_PER_DELTA, delta);
        {
            QMutexLocker l(&_imp->zoomCtxMutex);
            zoomFactor = _imp->zoomCtx.factor() * scaleFactor;
            if (zoomFactor <= zoomFactor_min) {
                zoomFactor = zoomFactor_min;
                scaleFactor = zoomFactor / _imp->zoomCtx.factor();
            } else if (zoomFactor > zoomFactor_max) {
                zoomFactor = zoomFactor_max;
                scaleFactor = zoomFactor / _imp->zoomCtx.factor();
            }
            _imp->zoomCtx.zoom(oldClick_opengl.x(), oldClick_opengl.y(), scaleFactor);
            _imp->zoomOrPannedSinceLastFit = true;
        }
        int zoomValue = (int)(100 * zoomFactor);
        if (zoomValue == 0) {
            zoomValue = 1; // sometimes, floor(100*0.01) makes 0
        }
        assert(zoomValue > 0);
        emit zoomChanged(zoomValue);

        //_imp->oldClick = newClick; // don't update oldClick! this is the zoom center
        _imp->viewerTab->getInternalNode()->renderCurrentFrame(false);
        
        //  else {
        mustRedraw = true;
        // }
        // no need to update the color picker or mouse posn: they should be unchanged
        break;
    }
    case eMouseStateDraggingRoiBottomEdge: {
        QMutexLocker l(&_imp->userRoIMutex);
        if ( (_imp->userRoI.y1 - dySinceLastMove) < _imp->userRoI.y2 ) {
            _imp->userRoI.y1 -= dySinceLastMove;
            l.unlock();
            if ( displayingImage() ) {
                _imp->viewerTab->getInternalNode()->renderCurrentFrame(false);
            }
            mustRedraw = true;
        }
        break;
    }
    case eMouseStateDraggingRoiLeftEdge: {
        QMutexLocker l(&_imp->userRoIMutex);
        if ( (_imp->userRoI.x1 - dxSinceLastMove) < _imp->userRoI.x2 ) {
            _imp->userRoI.x1 -= dxSinceLastMove;
            l.unlock();
            if ( displayingImage() ) {
                _imp->viewerTab->getInternalNode()->renderCurrentFrame(false);
            }
            mustRedraw = true;
        }
        break;
    }
    case eMouseStateDraggingRoiRightEdge: {
        QMutexLocker l(&_imp->userRoIMutex);
        if ( (_imp->userRoI.x2 - dxSinceLastMove) > _imp->userRoI.x1 ) {
            _imp->userRoI.x2 -= dxSinceLastMove;
            l.unlock();
            if ( displayingImage() ) {
                _imp->viewerTab->getInternalNode()->renderCurrentFrame(false);
            }
            mustRedraw = true;
        }
        break;
    }
    case eMouseStateDraggingRoiTopEdge: {
        QMutexLocker l(&_imp->userRoIMutex);
        if ( (_imp->userRoI.y2 - dySinceLastMove) > _imp->userRoI.y1 ) {
            _imp->userRoI.y2 -= dySinceLastMove;
            l.unlock();
            if ( displayingImage() ) {
                _imp->viewerTab->getInternalNode()->renderCurrentFrame(false);
            }
            mustRedraw = true;
        }
        break;
    }
    case eMouseStateDraggingRoiCross: {
        {
            QMutexLocker l(&_imp->userRoIMutex);
            _imp->userRoI.translate(-dxSinceLastMove,-dySinceLastMove);
        }
        if ( displayingImage() ) {
            _imp->viewerTab->getInternalNode()->renderCurrentFrame(false);
        }
        mustRedraw = true;
        break;
    }
    case eMouseStateDraggingRoiTopLeft: {
        QMutexLocker l(&_imp->userRoIMutex);
        if ( (_imp->userRoI.y2 - dySinceLastMove) > _imp->userRoI.y1 ) {
            _imp->userRoI.y2 -= dySinceLastMove;
        }
        if ( (_imp->userRoI.x1 - dxSinceLastMove) < _imp->userRoI.x2 ) {
            _imp->userRoI.x1 -= dxSinceLastMove;
        }
        l.unlock();
        if ( displayingImage() ) {
            _imp->viewerTab->getInternalNode()->renderCurrentFrame(false);
        }
        mustRedraw = true;
        break;
    }
    case eMouseStateDraggingRoiTopRight: {
        QMutexLocker l(&_imp->userRoIMutex);
        if ( (_imp->userRoI.y2 - dySinceLastMove) > _imp->userRoI.y1 ) {
            _imp->userRoI.y2 -= dySinceLastMove;
        }
        if ( (_imp->userRoI.x2 - dxSinceLastMove) > _imp->userRoI.x1 ) {
            _imp->userRoI.x2 -= dxSinceLastMove;
        }
        l.unlock();
        if ( displayingImage() ) {
            _imp->viewerTab->getInternalNode()->renderCurrentFrame(false);
        }
        mustRedraw = true;
        break;
    }
    case eMouseStateDraggingRoiBottomRight: {
        QMutexLocker l(&_imp->userRoIMutex);
        if ( (_imp->userRoI.x2 - dxSinceLastMove) > _imp->userRoI.x1 ) {
            _imp->userRoI.x2 -= dxSinceLastMove;
        }
        if ( (_imp->userRoI.y1 - dySinceLastMove) < _imp->userRoI.y2 ) {
            _imp->userRoI.y1 -= dySinceLastMove;
        }
        l.unlock();
        if ( displayingImage() ) {
            _imp->viewerTab->getInternalNode()->renderCurrentFrame(false);
        }
        mustRedraw = true;
        break;
    }
    case eMouseStateDraggingRoiBottomLeft: {
        if ( (_imp->userRoI.y1 - dySinceLastMove) < _imp->userRoI.y2 ) {
            _imp->userRoI.y1 -= dySinceLastMove;
        }
        if ( (_imp->userRoI.x1 - dxSinceLastMove) < _imp->userRoI.x2 ) {
            _imp->userRoI.x1 -= dxSinceLastMove;
        }
        _imp->userRoIMutex.unlock();
        if ( displayingImage() ) {
            _imp->viewerTab->getInternalNode()->renderCurrentFrame(false);
        }
        mustRedraw = true;
        break;
    }
    case eMouseStateDraggingWipeCenter: {
        QMutexLocker l(&_imp->wipeControlsMutex);
        _imp->wipeCenter.rx() -= dxSinceLastMove;
        _imp->wipeCenter.ry() -= dySinceLastMove;
        mustRedraw = true;
        break;
    }
    case eMouseStateDraggingWipeMixHandle: {
        QMutexLocker l(&_imp->wipeControlsMutex);
        double angle = std::atan2( zoomPos.y() - _imp->wipeCenter.y(), zoomPos.x() - _imp->wipeCenter.x() );
        double prevAngle = std::atan2( oldPosition_opengl.y() - _imp->wipeCenter.y(),
                                       oldPosition_opengl.x() - _imp->wipeCenter.x() );
        _imp->mixAmount -= (angle - prevAngle);
        _imp->mixAmount = std::max( 0.,std::min(_imp->mixAmount,1.) );
        mustRedraw = true;
        break;
    }
    case eMouseStateRotatingWipeHandle: {
        QMutexLocker l(&_imp->wipeControlsMutex);
        double angle = std::atan2( zoomPos.y() - _imp->wipeCenter.y(), zoomPos.x() - _imp->wipeCenter.x() );
        _imp->wipeAngle = angle;
        double mpi2 = M_PI / 2.;
        double closestPI2 = mpi2 * std::floor( (_imp->wipeAngle + M_PI / 4.) / mpi2 );
        if (std::fabs(_imp->wipeAngle - closestPI2) < 0.1) {
            // snap to closest multiple of PI / 2.
            _imp->wipeAngle = closestPI2;
        }

        mustRedraw = true;
        break;
    }
    case eMouseStatePickingColor: {
        pickColor( newClick.x(), newClick.y() );
        mustRedraw = true;
        break;
    }
    case eMouseStateBuildingPickerRectangle: {
        QPointF btmRight = _imp->pickerRect.bottomRight();
        btmRight.rx() -= dxSinceLastMove;
        btmRight.ry() -= dySinceLastMove;
        _imp->pickerRect.setBottomRight(btmRight);
        mustRedraw = true;
        break;
    }
    case eMouseStateSelecting: {
        _imp->refreshSelectionRectangle(zoomPos);
        mustRedraw = true;
        emit selectionRectangleChanged(false);
    }; break;
    default: {
        unsigned int mipMapLevel = getCurrentRenderScale();
        double scale = 1. / (1 << mipMapLevel);
        if ( _imp->overlay &&
             _imp->viewerTab->notifyOverlaysPenMotion(scale,scale, QMouseEventLocalPos(e), zoomPos, e) ) {
            mustRedraw = true;
        }
        break;
    }
    } // switch

    if (mustRedraw) {
        updateGL();
    }
    _imp->lastMousePosition = newClick;
    //FIXME: This is bugged, somehow we can't set our custom picker cursor...
//    if(_imp->viewerTab->getGui()->_projectGui->hasPickers()){
//        setCursor(appPTR->getColorPickerCursor());
//    }else{
//        unsetCursor();
//    }
    QGLWidget::mouseMoveEvent(e);
} // mouseMoveEvent

void
ViewerGL::mouseDoubleClickEvent(QMouseEvent* e)
{
    unsigned int mipMapLevel = getInternalNode()->getMipMapLevel();
    QPointF pos_opengl;
    {
        QMutexLocker l(&_imp->zoomCtxMutex);
        pos_opengl = _imp->zoomCtx.toZoomCoordinates( e->x(),e->y() );
    }
    double scale = 1. / (1 << mipMapLevel);
    if ( _imp->viewerTab->notifyOverlaysPenDoubleClick(scale,scale, QMouseEventLocalPos(e), pos_opengl, e) ) {
        updateGL();
    }
    QGLWidget::mouseDoubleClickEvent(e);
}

// used to update the information bar at the bottom of the viewer (not for the ctrl-click color picker)
void
ViewerGL::updateColorPicker(int textureIndex,
                            int x,
                            int y)
{
    if (_imp->pickerState != ePickerStateInactive || _imp->viewerTab->getGui()->isGUIFrozen()) {
        return;
    }

    // always running in the main thread
    assert( qApp && qApp->thread() == QThread::currentThread() );
    if ( !displayingImage() && _imp->infoViewer[textureIndex]->colorAndMouseVisible() ) {
        _imp->infoViewer[textureIndex]->hideColorAndMouseInfo();

        return;
    }

    QPoint pos;
    bool xInitialized = false;
    bool yInitialized = false;
    if (x != INT_MAX) {
        xInitialized = true;
        pos.setX(x);
    }
    if (y != INT_MAX) {
        yInitialized = true;
        pos.setY(y);
    }
    QPoint currentPos = mapFromGlobal( QCursor::pos() );
    if (!xInitialized) {
        pos.setX( currentPos.x() );
    }
    if (!yInitialized) {
        pos.setY( currentPos.y() );
    }
    float r,g,b,a;
    QPointF imgPosCanonical;
    {
        QMutexLocker l(&_imp->zoomCtxMutex);
        imgPosCanonical = _imp->zoomCtx.toZoomCoordinates( pos.x(), pos.y() );
    }
    bool linear = appPTR->getCurrentSettings()->getColorPickerLinear();
    bool picked = false;
    RectD rod = getRoD(textureIndex);
    RectD projectCanonical;
    _imp->getProjectFormatCanonical(projectCanonical);
    unsigned int mmLevel;
    if ( ( imgPosCanonical.x() >= rod.left() ) &&
         ( imgPosCanonical.x() < rod.right() ) &&
         ( imgPosCanonical.y() >= rod.bottom() ) &&
         ( imgPosCanonical.y() < rod.top() ) &&
         ( pos.x() >= 0) && ( pos.x() < width() ) &&
         ( pos.y() >= 0) && ( pos.y() < height() ) ) {
        ///if the clip to project format is enabled, make sure it is in the project format too
        bool clipping = isClippingImageToProjectWindow();
        if ( !clipping ||
             ( ( imgPosCanonical.x() >= projectCanonical.left() ) &&
               ( imgPosCanonical.x() < projectCanonical.right() ) &&
               ( imgPosCanonical.y() >= projectCanonical.bottom() ) &&
               ( imgPosCanonical.y() < projectCanonical.top() ) ) ) {
            //imgPos must be in canonical coordinates
            picked = getColorAt(imgPosCanonical.x(), imgPosCanonical.y(), linear, textureIndex, &r, &g, &b, &a,&mmLevel);
        }
    }
    if (!picked) {
        _imp->infoViewer[textureIndex]->setColorValid(false);
    } else {
        _imp->infoViewer[textureIndex]->setColorApproximated(mmLevel > 0);
        _imp->infoViewer[textureIndex]->setColorValid(true);
        if ( !_imp->infoViewer[textureIndex]->colorAndMouseVisible() ) {
            _imp->infoViewer[textureIndex]->showColorAndMouseInfo();
        }
        _imp->infoViewer[textureIndex]->setColor(r,g,b,a);
    }
} // updateColorPicker

void
ViewerGL::wheelEvent(QWheelEvent* e)
{
    // always running in the main thread
    assert( qApp && qApp->thread() == QThread::currentThread() );
    if (e->orientation() != Qt::Vertical) {
        return;
    }

    if (!_imp->viewerTab) {
        return;
    }
    Gui* gui = _imp->viewerTab->getGui();
    if (!gui) {
        return;
    }
    gui->selectNode(gui->getApp()->getNodeGui( _imp->viewerTab->getInternalNode()->getNode() ) );

    const double zoomFactor_min = 0.01;
    const double zoomFactor_max = 1024.;
    double zoomFactor;
    unsigned int oldMipMapLevel,newMipMapLevel;
    double scaleFactor = std::pow( NATRON_WHEEL_ZOOM_PER_DELTA, e->delta() );
    {
        QMutexLocker l(&_imp->zoomCtxMutex);
        QPointF zoomCenter = _imp->zoomCtx.toZoomCoordinates( e->x(), e->y() );
        zoomFactor = _imp->zoomCtx.factor() ;

        oldMipMapLevel = std::log(zoomFactor >= 1 ? 1 : std::pow( 2,-std::ceil(std::log(zoomFactor) / M_LN2) )) / M_LN2;

        zoomFactor*= scaleFactor;
        
        if (zoomFactor <= zoomFactor_min) {
            zoomFactor = zoomFactor_min;
            scaleFactor = zoomFactor / _imp->zoomCtx.factor();
        } else if (zoomFactor > zoomFactor_max) {
            zoomFactor = zoomFactor_max;
            scaleFactor = zoomFactor / _imp->zoomCtx.factor();
        }
        
        newMipMapLevel = std::log(zoomFactor >= 1 ? 1 : std::pow( 2,-std::ceil(std::log(zoomFactor) / M_LN2) )) / M_LN2;
        _imp->zoomCtx.zoom(zoomCenter.x(), zoomCenter.y(), scaleFactor);
        _imp->zoomOrPannedSinceLastFit = true;
    }
    int zoomValue = (int)(100 * zoomFactor);
    if (zoomValue == 0) {
        zoomValue = 1; // sometimes, floor(100*0.01) makes 0
    }
    assert(zoomValue > 0);
    emit zoomChanged(zoomValue);


    _imp->viewerTab->getInternalNode()->renderCurrentFrame(false);
    

    ///Clear green cached line so the user doesn't expect to see things in the cache
    ///since we're changing the zoom factor
    if (oldMipMapLevel != newMipMapLevel) {
        _imp->viewerTab->clearTimelineCacheLine();
    }
    updateGL();
}

void
ViewerGL::zoomSlot(int v)
{
    // always running in the main thread
    assert( qApp && qApp->thread() == QThread::currentThread() );

    assert(v > 0);
    double newZoomFactor = v / 100.;
    if (newZoomFactor < 0.01) {
        newZoomFactor = 0.01;
    } else if (newZoomFactor > 1024.) {
        newZoomFactor = 1024.;
    }
    unsigned int oldMipMapLevel,newMipMapLevel;
    newMipMapLevel = std::log(newZoomFactor >= 1 ? 1 :
                              std::pow( 2,-std::ceil(std::log(newZoomFactor) / M_LN2) )) / M_LN2;
    {
        QMutexLocker l(&_imp->zoomCtxMutex);
        oldMipMapLevel = std::log(_imp->zoomCtx.factor() >= 1 ? 1 :
                                  std::pow( 2,-std::ceil(std::log(_imp->zoomCtx.factor()) / M_LN2) )) / M_LN2;
        double scale = newZoomFactor / _imp->zoomCtx.factor();
        double centerX = ( _imp->zoomCtx.left() + _imp->zoomCtx.right() ) / 2.;
        double centerY = ( _imp->zoomCtx.top() + _imp->zoomCtx.bottom() ) / 2.;
        _imp->zoomCtx.zoom(centerX, centerY, scale);
        _imp->zoomOrPannedSinceLastFit = true;
    }
    ///Clear green cached line so the user doesn't expect to see things in the cache
    ///since we're changing the zoom factor
    if (newMipMapLevel != oldMipMapLevel) {
        _imp->viewerTab->clearTimelineCacheLine();
    }
    
    _imp->viewerTab->getInternalNode()->renderCurrentFrame(false);
   
}

void
ViewerGL::zoomSlot(QString str)
{
    // always running in the main thread
    assert( qApp && qApp->thread() == QThread::currentThread() );
    str.remove( QChar('%') );
    int v = str.toInt();
    assert(v > 0);
    zoomSlot(v);
}

void
ViewerGL::fitImageToFormat()
{
    // always running in the main thread
    assert( qApp && qApp->thread() == QThread::currentThread() );
    // size in Canonical = Zoom coordinates !
    double h = _imp->projectFormat.height();
    double w = _imp->projectFormat.width() * _imp->projectFormat.getPixelAspectRatio();

    assert(h > 0. && w > 0.);

    double old_zoomFactor;
    double zoomFactor;
    {
        QMutexLocker(&_imp->zoomCtxMutex);
        old_zoomFactor = _imp->zoomCtx.factor();
        // set the PAR first
        //_imp->zoomCtx.setZoom(0., 0., 1., 1.);
        // leave 4% of margin around
        _imp->zoomCtx.fit(-0.02 * w, 1.02 * w, -0.02 * h, 1.02 * h);
        zoomFactor = _imp->zoomCtx.factor();
        _imp->zoomOrPannedSinceLastFit = false;
    }
    _imp->oldClick = QPoint(); // reset mouse posn

    if (old_zoomFactor != zoomFactor) {
        int zoomFactorInt = zoomFactor  * 100;
        if (zoomFactorInt == 0) {
            zoomFactorInt = 1;
        }
        emit zoomChanged(zoomFactorInt);
    }
    ///Clear green cached line so the user doesn't expect to see things in the cache
    ///since we're changing the zoom factor
    _imp->viewerTab->clearTimelineCacheLine();
}

/**
 *@brief Turns on the overlays on the viewer.
 **/
void
ViewerGL::turnOnOverlay()
{
    // always running in the main thread
    assert( qApp && qApp->thread() == QThread::currentThread() );
    _imp->overlay = true;
}

/**
 *@brief Turns off the overlays on the viewer.
 **/
void
ViewerGL::turnOffOverlay()
{
    // always running in the main thread
    assert( qApp && qApp->thread() == QThread::currentThread() );
    _imp->overlay = false;
}

void
ViewerGL::setInfoViewer(InfoViewerWidget* i,
                        int textureIndex )
{
    // always running in the main thread
    assert( qApp && qApp->thread() == QThread::currentThread() );
    _imp->infoViewer[textureIndex] = i;
}

void
ViewerGL::disconnectViewer()
{
    // always running in the main thread
    assert( qApp && qApp->thread() == QThread::currentThread() );
    if ( displayingImage() ) {
        setRegionOfDefinition(_imp->blankViewerInfo.getRoD(),_imp->blankViewerInfo.getDisplayWindow().getPixelAspectRatio(),0);
        setRegionOfDefinition(_imp->blankViewerInfo.getRoD(),_imp->blankViewerInfo.getDisplayWindow().getPixelAspectRatio(),1);
    }
    resetWipeControls();
    clearViewer();
}

/* The dataWindow of the currentFrame(BBOX) in canonical coordinates */
const RectD&
ViewerGL::getRoD(int textureIndex) const
{
    // always running in the main thread
    assert( qApp && qApp->thread() == QThread::currentThread() );

    return _imp->currentViewerInfo[textureIndex].getRoD();
}

/*The displayWindow of the currentFrame(Resolution)
   This is the same for both as we only use the display window as to indicate the project window.*/
Format
ViewerGL::getDisplayWindow() const
{
    // always running in the main thread
    assert( qApp && qApp->thread() == QThread::currentThread() );

    return _imp->currentViewerInfo[0].getDisplayWindow();
}

void
ViewerGL::setRegionOfDefinition(const RectD & rod,
                                double par,
                                int textureIndex)
{
    // always running in the main thread
    assert( qApp && qApp->thread() == QThread::currentThread() );
    if (!_imp->viewerTab->getGui()) {
        return;
    }
    
    RectI pixelRoD;
    rod.toPixelEnclosing(0, par, &pixelRoD);
    
    _imp->currentViewerInfo[textureIndex].setRoD(rod);
    if (_imp->infoViewer[textureIndex] && !_imp->viewerTab->getGui()->isGUIFrozen()) {
        _imp->infoViewer[textureIndex]->setDataWindow(pixelRoD);
    }
    

    QString left,btm,right,top;
    left.setNum(pixelRoD.left());
    btm.setNum(pixelRoD.bottom());
    right.setNum(pixelRoD.right());
    top.setNum(pixelRoD.top());


    _imp->currentViewerInfo_btmLeftBBOXoverlay[textureIndex].clear();
    _imp->currentViewerInfo_btmLeftBBOXoverlay[textureIndex].append(left);
    _imp->currentViewerInfo_btmLeftBBOXoverlay[textureIndex].append(",");
    _imp->currentViewerInfo_btmLeftBBOXoverlay[textureIndex].append(btm);
    _imp->currentViewerInfo_topRightBBOXoverlay[textureIndex].clear();
    _imp->currentViewerInfo_topRightBBOXoverlay[textureIndex].append(right);
    _imp->currentViewerInfo_topRightBBOXoverlay[textureIndex].append(",");
    _imp->currentViewerInfo_topRightBBOXoverlay[textureIndex].append(top);
}

void
ViewerGL::onProjectFormatChangedInternal(const Format & format,bool triggerRender)
{
    // always running in the main thread
    assert( qApp && qApp->thread() == QThread::currentThread() );
    
    if (!_imp->viewerTab->getGui()) {
        return;
    }
    RectD canonicalFormat = format.toCanonicalFormat();
    
    _imp->blankViewerInfo.setDisplayWindow(format);
    _imp->blankViewerInfo.setRoD(canonicalFormat);
    for (int i = 0; i < 2; ++i) {
        if (_imp->infoViewer[i]) {
            _imp->infoViewer[i]->setResolution(format);
        }
    }
    {
        QMutexLocker k(&_imp->projectFormatMutex);
        _imp->projectFormat = format;
    }
    _imp->currentViewerInfo_resolutionOverlay.clear();
    _imp->currentViewerInfo_resolutionOverlay.append( QString::number(format.width() ) );
    _imp->currentViewerInfo_resolutionOverlay.append("x");
    _imp->currentViewerInfo_resolutionOverlay.append( QString::number(format.height() ) );
    
    bool loadingProject = _imp->viewerTab->getGui()->getApp()->getProject()->isLoadingProject();
    if ( !loadingProject && triggerRender) {
        fitImageToFormat();
        if ( _imp->viewerTab->getInternalNode()) {
            _imp->viewerTab->getInternalNode()->renderCurrentFrame(false);
        }
    }
    
    
    
    if (!_imp->isUserRoISet) {
        {
            QMutexLocker l(&_imp->userRoIMutex);
            _imp->userRoI = canonicalFormat;
        }
        _imp->isUserRoISet = true;
    }
    if (!loadingProject) {
        updateGL();
    }

}

void
ViewerGL::onProjectFormatChanged(const Format & format)
{
    onProjectFormatChangedInternal(format, true);
}

void
ViewerGL::setClipToDisplayWindow(bool b)
{
    // always running in the main thread
    assert( qApp && qApp->thread() == QThread::currentThread() );
    {
        QMutexLocker l(&_imp->clipToDisplayWindowMutex);
        _imp->clipToDisplayWindow = b;
    }
    ViewerInstance* viewer = _imp->viewerTab->getInternalNode();
    assert(viewer);
    if ( viewer->getUiContext() && !_imp->viewerTab->getGui()->getApp()->getProject()->isLoadingProject() ) {
        _imp->viewerTab->getInternalNode()->renderCurrentFrame(false);
    }
}

bool
ViewerGL::isClippingImageToProjectWindow() const
{
    // MT-SAFE
    QMutexLocker l(&_imp->clipToDisplayWindowMutex);

    return _imp->clipToDisplayWindow;
}

/*display black in the viewer*/
void
ViewerGL::clearViewer()
{
    // always running in the main thread
    assert( qApp && qApp->thread() == QThread::currentThread() );
    _imp->activeTextures[0] = 0;
    _imp->activeTextures[1] = 0;
    updateGL();
}

/*overload of QT enter/leave/resize events*/
void
ViewerGL::focusInEvent(QFocusEvent* e)
{
    // always running in the main thread
    assert( qApp && qApp->thread() == QThread::currentThread() );
    ///The app is closing don't do anything
    if ( !_imp->viewerTab->getGui() ) {
        return;
    }
    double scale = 1. / (1 << getCurrentRenderScale());
    if ( _imp->viewerTab->notifyOverlaysFocusGained(scale,scale) ) {
        updateGL();
    }
    QGLWidget::focusInEvent(e);
}

void
ViewerGL::focusOutEvent(QFocusEvent* e)
{
    // always running in the main thread
    assert( qApp && qApp->thread() == QThread::currentThread() );

    if ( !_imp->viewerTab->getGui() ) {
        return;
    }

    double scale = 1. / (1 << getCurrentRenderScale());
    if ( _imp->viewerTab->notifyOverlaysFocusLost(scale,scale) ) {
        updateGL();
    }
    QGLWidget::focusOutEvent(e);
}

void
ViewerGL::enterEvent(QEvent* e)
{
    // always running in the main thread
    assert( qApp && qApp->thread() == QThread::currentThread() );
    
    QWidget* currentFocus = qApp->focusWidget();
    
    bool canSetFocus = !currentFocus ||
    dynamic_cast<ViewerGL*>(currentFocus) ||
    dynamic_cast<CurveWidget*>(currentFocus) ||
    dynamic_cast<Histogram*>(currentFocus) ||
    dynamic_cast<NodeGraph*>(currentFocus) ||
    dynamic_cast<QToolButton*>(currentFocus) ||
    currentFocus->objectName() == "Properties" ||
    currentFocus->objectName() == "SettingsPanel" ||
    currentFocus->objectName() == "qt_tabwidget_tabbar";
    
    if (canSetFocus) {
        setFocus();
    }
    QWidget::enterEvent(e);
}

void
ViewerGL::leaveEvent(QEvent* e)
{
    // always running in the main thread
    assert( qApp && qApp->thread() == QThread::currentThread() );
    _imp->infoViewer[0]->hideColorAndMouseInfo();
    _imp->infoViewer[1]->hideColorAndMouseInfo();
    QGLWidget::leaveEvent(e);
}

void
ViewerGL::resizeEvent(QResizeEvent* e)
{ // public to hack the protected field
  // always running in the main thread
    assert( qApp && qApp->thread() == QThread::currentThread() );
    QGLWidget::resizeEvent(e);
}

void
ViewerGL::keyPressEvent(QKeyEvent* e)
{
    // always running in the main thread
    assert( qApp && qApp->thread() == QThread::currentThread() );
    
    Qt::KeyboardModifiers modifiers = e->modifiers();
    Qt::Key key = (Qt::Key)e->key();
    bool accept = false;

    if (key == Qt::Key_Escape) {
        QGLWidget::keyPressEvent(e);
    }
    
    if ( isKeybind(kShortcutGroupViewer, kShortcutIDActionHideOverlays, modifiers, key) ) {
        toggleOverlays();
    } else if (isKeybind(kShortcutGroupViewer, kShortcutIDToggleWipe, modifiers, key)) {
        toggleWipe();
    } else if ( isKeybind(kShortcutGroupViewer, kShortcutIDActionHideAll, modifiers, key) ) {
        _imp->viewerTab->hideAllToolbars();
        accept = true;
    } else if ( isKeybind(kShortcutGroupViewer, kShortcutIDActionShowAll, modifiers, key) ) {
        _imp->viewerTab->showAllToolbars();
        accept = true;
    } else if ( isKeybind(kShortcutGroupViewer, kShortcutIDActionHidePlayer, modifiers, key) ) {
        _imp->viewerTab->togglePlayerVisibility();
        accept = true;
    } else if ( isKeybind(kShortcutGroupViewer, kShortcutIDActionHideTimeline, modifiers, key) ) {
        _imp->viewerTab->toggleTimelineVisibility();
        accept = true;
    } else if ( isKeybind(kShortcutGroupViewer, kShortcutIDActionHideInfobar, modifiers, key) ) {
        _imp->viewerTab->toggleInfobarVisbility();
        accept = true;
    } else if ( isKeybind(kShortcutGroupViewer, kShortcutIDActionHideLeft, modifiers, key) ) {
        _imp->viewerTab->toggleLeftToolbarVisiblity();
        accept = true;
    } else if ( isKeybind(kShortcutGroupViewer, kShortcutIDActionHideRight, modifiers, key) ) {
        _imp->viewerTab->toggleRightToolbarVisibility();
        accept = true;
    } else if ( isKeybind(kShortcutGroupViewer, kShortcutIDActionHideTop, modifiers, key) ) {
        _imp->viewerTab->toggleTopToolbarVisibility();
        accept = true;
    } else {
        QGLWidget::keyPressEvent(e);
    }

    double scale = 1. / (1 << getCurrentRenderScale());
    if ( e->isAutoRepeat() ) {
        if ( _imp->viewerTab->notifyOverlaysKeyRepeat(scale, scale, e) ) {
            accept = true;
            updateGL();
        }
    } else {
        if ( _imp->viewerTab->notifyOverlaysKeyDown(scale, scale, e) ) {
            accept = true;
            updateGL();
        }
    }
    if (accept) {
        e->accept();
    } else {
        e->ignore();
    }
}

void
ViewerGL::keyReleaseEvent(QKeyEvent* e)
{
    // always running in the main thread
    assert( qApp && qApp->thread() == QThread::currentThread() );
    if (!_imp->viewerTab->getGui()) {
        return;
    }
    double scale = 1. / (1 << getCurrentRenderScale());
    if ( _imp->viewerTab->notifyOverlaysKeyUp(scale, scale, e) ) {
        updateGL();
    }
}

OpenGLViewerI::BitDepthEnum
ViewerGL::getBitDepth() const
{
    // MT-SAFE
    ///supportsGLSL is set on the main thread only once on startup, it doesn't need to be protected.
    if (!_imp->supportsGLSL) {
        return OpenGLViewerI::eBitDepthByte;
    } else {
        // FIXME: casting an int to an enum!

        ///the bitdepth value is locked by the knob holding that value itself.
        return (OpenGLViewerI::BitDepthEnum)appPTR->getCurrentSettings()->getViewersBitDepth();
    }
}

void
ViewerGL::populateMenu()
{
    
    
    // always running in the main thread
    assert( qApp && qApp->thread() == QThread::currentThread() );
    _imp->menu->clear();
    QAction* displayOverlaysAction = new ActionWithShortcut(kShortcutGroupViewer,kShortcutIDActionHideOverlays,kShortcutDescActionHideOverlays, _imp->menu);

    displayOverlaysAction->setCheckable(true);
    displayOverlaysAction->setChecked(_imp->overlay);
    QObject::connect( displayOverlaysAction,SIGNAL( triggered() ),this,SLOT( toggleOverlays() ) );
    
    
    QAction* toggleWipe = new ActionWithShortcut(kShortcutGroupViewer,kShortcutIDToggleWipe,kShortcutDescToggleWipe, _imp->menu);
    toggleWipe->setCheckable(true);
    toggleWipe->setChecked(getViewerTab()->getCompositingOperator() != Natron::eViewerCompositingOperatorNone);
    QObject::connect( toggleWipe,SIGNAL( triggered() ),this,SLOT( toggleWipe() ) );
    _imp->menu->addAction(toggleWipe);
    
    QMenu* showHideMenu = new QMenu(tr("Show/Hide"),_imp->menu);
    showHideMenu->setFont(QFont(appFont,appFontSize));
    _imp->menu->addAction(showHideMenu->menuAction());
    
    
    QAction* showHidePlayer,*showHideLeftToolbar,*showHideRightToolbar,*showHideTopToolbar,*showHideInfobar,*showHideTimeline;
    QAction* showAll,*hideAll;
    
    showHidePlayer = new ActionWithShortcut(kShortcutGroupViewer,kShortcutIDActionHidePlayer,kShortcutDescActionHidePlayer,
                                                showHideMenu);

    showHideLeftToolbar = new ActionWithShortcut(kShortcutGroupViewer,kShortcutIDActionHideLeft,kShortcutDescActionHideLeft,
                                                 showHideMenu);
    showHideRightToolbar = new ActionWithShortcut(kShortcutGroupViewer,kShortcutIDActionHideRight,kShortcutDescActionHideRight,
                                                  showHideMenu);
    showHideTopToolbar = new ActionWithShortcut(kShortcutGroupViewer,kShortcutIDActionHideTop,kShortcutDescActionHideTop,
                                                showHideMenu);
    showHideInfobar = new ActionWithShortcut(kShortcutGroupViewer,kShortcutIDActionHideInfobar,kShortcutDescActionHideInfobar,
                                             showHideMenu);
    showHideTimeline = new ActionWithShortcut(kShortcutGroupViewer,kShortcutIDActionHideTimeline,kShortcutDescActionHideTimeline,
                                              showHideMenu);
   
    
    showAll = new ActionWithShortcut(kShortcutGroupViewer,kShortcutIDActionShowAll,kShortcutDescActionShowAll,
                                     showHideMenu);

    hideAll = new ActionWithShortcut(kShortcutGroupViewer,kShortcutIDActionHideAll,kShortcutDescActionHideAll,
                                     showHideMenu);
    
    QObject::connect(showHidePlayer,SIGNAL(triggered()),_imp->viewerTab,SLOT(togglePlayerVisibility()));
    QObject::connect(showHideLeftToolbar,SIGNAL(triggered()),_imp->viewerTab,SLOT(toggleLeftToolbarVisiblity()));
    QObject::connect(showHideRightToolbar,SIGNAL(triggered()),_imp->viewerTab,SLOT(toggleRightToolbarVisibility()));
    QObject::connect(showHideTopToolbar,SIGNAL(triggered()),_imp->viewerTab,SLOT(toggleTopToolbarVisibility()));
    QObject::connect(showHideInfobar,SIGNAL(triggered()),_imp->viewerTab,SLOT(toggleInfobarVisbility()));
    QObject::connect(showHideTimeline,SIGNAL(triggered()),_imp->viewerTab,SLOT(toggleTimelineVisibility()));
    QObject::connect(showAll,SIGNAL(triggered()),_imp->viewerTab,SLOT(showAllToolbars()));
    QObject::connect(hideAll,SIGNAL(triggered()),_imp->viewerTab,SLOT(hideAllToolbars()));
    
    showHideMenu->addAction(showHidePlayer);
    showHideMenu->addAction(showHideTimeline);
    showHideMenu->addAction(showHideInfobar);
    showHideMenu->addAction(showHideLeftToolbar);
    showHideMenu->addAction(showHideRightToolbar);
    showHideMenu->addAction(showHideTopToolbar);
    showHideMenu->addAction(showAll);
    showHideMenu->addAction(hideAll);
    
    _imp->menu->addAction(displayOverlaysAction);
}

void
ViewerGL::renderText(double x,
                     double y,
                     const QString &string,
                     const QColor & color,
                     const QFont & font)
{
    // always running in the main thread
    assert( qApp && qApp->thread() == QThread::currentThread() );
    assert( QGLContext::currentContext() == context() );

    if ( string.isEmpty() ) {
        return;
    }
    {
        //GLProtectAttrib a(GL_TRANSFORM_BIT); // GL_MODELVIEW is active by default
        GLProtectMatrix p(GL_PROJECTION);
        glLoadIdentity();
        double h = (double)height();
        double w = (double)width();
        /*we put the ortho proj to the widget coords, draw the elements and revert back to the old orthographic proj.*/
        glOrtho(0, w, 0, h, 1, -1);
        GLProtectMatrix pmv(GL_MODELVIEW);
        glLoadIdentity();

        QPointF pos;
        {
            QMutexLocker l(&_imp->zoomCtxMutex);
            pos = _imp->zoomCtx.toWidgetCoordinates(x, y);
        }
        glCheckError();
        _imp->textRenderer.renderText(pos.x(),h - pos.y(),string,color,font);
        glCheckError();
    } // GLProtectAttrib a(GL_TRANSFORM_BIT);
}

void
ViewerGL::updatePersistentMessageToWidth(int w)
{
    // always running in the main thread
    assert( qApp && qApp->thread() == QThread::currentThread() );
    
    if (!_imp->viewerTab || !_imp->viewerTab->getGui()) {
        return;
    }
    
    std::list<boost::shared_ptr<Natron::Node> >  nodes;
    _imp->viewerTab->getGui()->getNodesEntitledForOverlays(nodes);
    
    _imp->persistentMessages.clear();
    QStringList allMessages;
    
    int type = 0;
    ///Draw overlays in reverse order of appearance
    std::list<boost::shared_ptr<Natron::Node> >::reverse_iterator next = nodes.rbegin();
    if (!nodes.empty()) {
        ++next;
    }
    int nbNonEmpty = 0;
    for (std::list<boost::shared_ptr<Natron::Node> >::reverse_iterator it = nodes.rbegin(); it != nodes.rend(); ++it) {
        QString mess;
        int nType;
        (*it)->getPersistentMessage(&mess, &nType);
        if (!mess.isEmpty()) {
             allMessages.append(mess);
            ++nbNonEmpty;
        }
        if (next != nodes.rend()) {
            ++next;
        }
        
        if (!mess.isEmpty()) {
            type = (nbNonEmpty == 1 && nType == 2) ? 2 : 1;
        }
    }
    _imp->persistentMessageType = type;
    
    QFontMetrics fm(*_imp->textFont);
    
    for (int i = 0; i < allMessages.size(); ++i) {
        QStringList wordWrapped = wordWrap(fm,allMessages[i], w - PERSISTENT_MESSAGE_LEFT_OFFSET_PIXELS);
        for (int j = 0; j < wordWrapped.size(); ++j) {
            _imp->persistentMessages.push_back(wordWrapped[j]);
        }
    }
    
    _imp->displayPersistentMessage = !_imp->persistentMessages.isEmpty();
    updateGL();
}

void
ViewerGL::updatePersistentMessage()
{
    updatePersistentMessageToWidth(width() - 20);
}

void
ViewerGL::getProjection(double *zoomLeft,
                        double *zoomBottom,
                        double *zoomFactor,
                        double *zoomAspectRatio) const
{
    // MT-SAFE
    QMutexLocker l(&_imp->zoomCtxMutex);

    *zoomLeft = _imp->zoomCtx.left();
    *zoomBottom = _imp->zoomCtx.bottom();
    *zoomFactor = _imp->zoomCtx.factor();
    *zoomAspectRatio = _imp->zoomCtx.aspectRatio();
}

void
ViewerGL::setProjection(double zoomLeft,
                        double zoomBottom,
                        double zoomFactor,
                        double zoomAspectRatio)
{
    // always running in the main thread
    assert( qApp && qApp->thread() == QThread::currentThread() );
    QMutexLocker l(&_imp->zoomCtxMutex);
    _imp->zoomCtx.setZoom(zoomLeft, zoomBottom, zoomFactor, zoomAspectRatio);
}

void
ViewerGL::setUserRoIEnabled(bool b)
{
    // always running in the main thread
    assert( qApp && qApp->thread() == QThread::currentThread() );
    {
        QMutexLocker(&_imp->userRoIMutex);
        _imp->userRoIEnabled = b;
    }
    if ( displayingImage() ) {
        _imp->viewerTab->getInternalNode()->renderCurrentFrame(false);
    }
    update();
}

bool
ViewerGL::isNearByUserRoITopEdge(const RectD & roi,
                                 const QPointF & zoomPos,
                                 double zoomScreenPixelWidth,
                                 double zoomScreenPixelHeight)
{
    // always running in the main thread
    assert( qApp && qApp->thread() == QThread::currentThread() );
    double length = std::min(roi.x2 - roi.x1 - 10, (USER_ROI_CLICK_TOLERANCE * zoomScreenPixelWidth) * 2);
    RectD r(roi.x1 + length / 2,
            roi.y2 - USER_ROI_CLICK_TOLERANCE * zoomScreenPixelHeight,
            roi.x2 - length / 2,
            roi.y2 + USER_ROI_CLICK_TOLERANCE * zoomScreenPixelHeight);

    return r.contains( zoomPos.x(), zoomPos.y() );
}

bool
ViewerGL::isNearByUserRoIRightEdge(const RectD & roi,
                                   const QPointF & zoomPos,
                                   double zoomScreenPixelWidth,
                                   double zoomScreenPixelHeight)
{
    // always running in the main thread
    assert( qApp && qApp->thread() == QThread::currentThread() );
    double length = std::min(roi.y2 - roi.y1 - 10, (USER_ROI_CLICK_TOLERANCE * zoomScreenPixelHeight) * 2);
    RectD r(roi.x2 - USER_ROI_CLICK_TOLERANCE * zoomScreenPixelWidth,
            roi.y1 + length / 2,
            roi.x2 + USER_ROI_CLICK_TOLERANCE * zoomScreenPixelWidth,
            roi.y2 - length / 2);

    return r.contains( zoomPos.x(), zoomPos.y() );
}

bool
ViewerGL::isNearByUserRoILeftEdge(const RectD & roi,
                                  const QPointF & zoomPos,
                                  double zoomScreenPixelWidth,
                                  double zoomScreenPixelHeight)
{
    // always running in the main thread
    assert( qApp && qApp->thread() == QThread::currentThread() );
    double length = std::min(roi.y2 - roi.y1 - 10, (USER_ROI_CLICK_TOLERANCE * zoomScreenPixelHeight) * 2);
    RectD r(roi.x1 - USER_ROI_CLICK_TOLERANCE * zoomScreenPixelWidth,
            roi.y1 + length / 2,
            roi.x1 + USER_ROI_CLICK_TOLERANCE * zoomScreenPixelWidth,
            roi.y2 - length / 2);

    return r.contains( zoomPos.x(), zoomPos.y() );
}

bool
ViewerGL::isNearByUserRoIBottomEdge(const RectD & roi,
                                    const QPointF & zoomPos,
                                    double zoomScreenPixelWidth,
                                    double zoomScreenPixelHeight)
{
    // always running in the main thread
    assert( qApp && qApp->thread() == QThread::currentThread() );
    double length = std::min(roi.x2 - roi.x1 - 10, (USER_ROI_CLICK_TOLERANCE * zoomScreenPixelWidth) * 2);
    RectD r(roi.x1 + length / 2,
            roi.y1 - USER_ROI_CLICK_TOLERANCE * zoomScreenPixelHeight,
            roi.x2 - length / 2,
            roi.y1 + USER_ROI_CLICK_TOLERANCE * zoomScreenPixelHeight);

    return r.contains( zoomPos.x(), zoomPos.y() );
}

bool
ViewerGL::isNearByUserRoI(double x,
                          double y,
                          const QPointF & zoomPos,
                          double zoomScreenPixelWidth,
                          double zoomScreenPixelHeight)
{
    // always running in the main thread
    assert( qApp && qApp->thread() == QThread::currentThread() );
    RectD r(x - USER_ROI_CROSS_RADIUS * zoomScreenPixelWidth,
            y - USER_ROI_CROSS_RADIUS * zoomScreenPixelHeight,
            x + USER_ROI_CROSS_RADIUS * zoomScreenPixelWidth,
            y + USER_ROI_CROSS_RADIUS * zoomScreenPixelHeight);

    return r.contains( zoomPos.x(), zoomPos.y() );
}

bool
ViewerGL::isUserRegionOfInterestEnabled() const
{
    // MT-SAFE
    QMutexLocker(&_imp->userRoIMutex);

    return _imp->userRoIEnabled;
}

RectD
ViewerGL::getUserRegionOfInterest() const
{
    // MT-SAFE
    QMutexLocker(&_imp->userRoIMutex);

    return _imp->userRoI;
}

void
ViewerGL::setUserRoI(const RectD & r)
{
    // MT-SAFE
    QMutexLocker(&_imp->userRoIMutex);
    _imp->userRoI = r;
}

/**
 * @brief Swap the OpenGL buffers.
 **/
void
ViewerGL::swapOpenGLBuffers()
{
    // always running in the main thread
    assert( qApp && qApp->thread() == QThread::currentThread() );
    swapBuffers();
}

/**
 * @brief Repaint
 **/
void
ViewerGL::redraw()
{
    // always running in the main thread
    assert( qApp && qApp->thread() == QThread::currentThread() );
    updateGL();
}

/**
 * @brief Returns the width and height of the viewport in window coordinates.
 **/
void
ViewerGL::getViewportSize(double &width,
                          double &height) const
{
    // always running in the main thread
    assert( qApp && qApp->thread() == QThread::currentThread() );
    QMutexLocker l(&_imp->zoomCtxMutex);
    width = _imp->zoomCtx.screenWidth();
    height = _imp->zoomCtx.screenHeight();
}

/**
 * @brief Returns the pixel scale of the viewport.
 **/
void
ViewerGL::getPixelScale(double & xScale,
                        double & yScale) const
{
    // always running in the main thread
    assert( qApp && qApp->thread() == QThread::currentThread() );
    QMutexLocker l(&_imp->zoomCtxMutex);
    xScale = _imp->zoomCtx.screenPixelWidth();
    yScale = _imp->zoomCtx.screenPixelHeight();
}

/**
 * @brief Returns the colour of the background (i.e: clear color) of the viewport.
 **/
void
ViewerGL::getBackgroundColour(double &r,
                              double &g,
                              double &b) const
{
    // always running in the main thread
    assert( qApp && qApp->thread() == QThread::currentThread() );
    r = _imp->clearColor.redF();
    g = _imp->clearColor.greenF();
    b = _imp->clearColor.blueF();
}

void
ViewerGL::makeOpenGLcontextCurrent()
{
    // always running in the main thread
    assert( qApp && qApp->thread() == QThread::currentThread() );
    makeCurrent();
}

void
ViewerGL::onViewerNodeNameChanged(const QString & name)
{
    // always running in the main thread
    assert( qApp && qApp->thread() == QThread::currentThread() );
    _imp->viewerTab->getGui()->unregisterTab(_imp->viewerTab);
    TabWidget* parent = dynamic_cast<TabWidget*>( _imp->viewerTab->parentWidget() );
    if (parent) {
        parent->setTabName(_imp->viewerTab, name);
    }
    _imp->viewerTab->getGui()->registerTab(_imp->viewerTab);
}

void
ViewerGL::removeGUI()
{
    // always running in the main thread
    assert( qApp && qApp->thread() == QThread::currentThread() );
    if ( _imp->viewerTab->getGui() ) {
        _imp->viewerTab->discardInternalNodePointer();
        _imp->viewerTab->getGui()->removeViewerTab(_imp->viewerTab, true,true);
    }
}

int
ViewerGL::getCurrentView() const
{
    // MT-SAFE

    ///protected in viewerTab (which is const)
    return _imp->viewerTab->getCurrentView();
}

ViewerInstance*
ViewerGL::getInternalNode() const
{
    return _imp->viewerTab->getInternalNode();
}

ViewerTab*
ViewerGL::getViewerTab() const
{
    return _imp->viewerTab;
}

// used for the ctrl-click color picker (not the information bar at the bottom of the viewer)
bool
ViewerGL::pickColor(double x,
                    double y)
{
    float r,g,b,a;
    QPointF imgPos;
    {
        QMutexLocker l(&_imp->zoomCtxMutex);
        imgPos = _imp->zoomCtx.toZoomCoordinates(x, y);
    }

    _imp->lastPickerPos = imgPos;
    bool linear = appPTR->getCurrentSettings()->getColorPickerLinear();
    bool ret = false;
    for (int i = 0; i < 2; ++i) {
        // imgPos must be in canonical coordinates
        unsigned int mmLevel;
        bool picked = getColorAt(imgPos.x(), imgPos.y(), linear, i, &r, &g, &b, &a,&mmLevel);
        if (picked) {
            if (i == 0) {
                QColor pickerColor;
                pickerColor.setRedF( Natron::clamp(r) );
                pickerColor.setGreenF( Natron::clamp(g) );
                pickerColor.setBlueF( Natron::clamp(b) );
                pickerColor.setAlphaF( Natron::clamp(a) );
                _imp->viewerTab->getGui()->setColorPickersColor(pickerColor);
            }
            _imp->infoViewer[i]->setColorApproximated(mmLevel > 0);
            _imp->infoViewer[i]->setColorValid(true);
            if ( !_imp->infoViewer[i]->colorAndMouseVisible() ) {
                _imp->infoViewer[i]->showColorAndMouseInfo();
            }
            _imp->infoViewer[i]->setColor(r,g,b,a);
            ret = true;
        } else {
            _imp->infoViewer[i]->setColorValid(false);
        }
    }

    return ret;
}

void
ViewerGL::updateInfoWidgetColorPicker(const QPointF & imgPos,
                                      const QPoint & widgetPos,
                                      int width,
                                      int height,
                                      const RectD & rod, // in canonical coordinates
                                      const RectD & dispW, // in canonical coordinates
                                      int texIndex)
{
    if (_imp->viewerTab->getGui()->isGUIFrozen()) {
        return;
    }
    
    if ( _imp->activeTextures[texIndex] &&
         ( imgPos.x() >= rod.left() ) &&
         ( imgPos.x() < rod.right() ) &&
         ( imgPos.y() >= rod.bottom() ) &&
         ( imgPos.y() < rod.top() ) &&
         ( widgetPos.x() >= 0) && ( widgetPos.x() < width) &&
         ( widgetPos.y() >= 0) && ( widgetPos.y() < height) ) {
        ///if the clip to project format is enabled, make sure it is in the project format too
        if ( isClippingImageToProjectWindow() &&
             ( ( imgPos.x() < dispW.left() ) ||
               ( imgPos.x() >= dispW.right() ) ||
               ( imgPos.y() < dispW.bottom() ) ||
               ( imgPos.y() >= dispW.top() ) ) ) {
            if ( _imp->infoViewer[texIndex]->colorAndMouseVisible() ) {
                _imp->infoViewer[texIndex]->hideColorAndMouseInfo();
            }
        } else {
            if (_imp->pickerState == ePickerStateInactive) {
                //if ( !_imp->viewerTab->getInternalNode()->getRenderEngine()->hasThreadsWorking() ) {
                    updateColorPicker( texIndex,widgetPos.x(),widgetPos.y() );
               // }
            } else if ( ( _imp->pickerState == ePickerStatePoint) || ( _imp->pickerState == ePickerStateRectangle) ) {
                if ( !_imp->infoViewer[texIndex]->colorAndMouseVisible() ) {
                    _imp->infoViewer[texIndex]->showColorAndMouseInfo();
                }
            } else {
                ///unkwn state
                assert(false);
            }
            double par = _imp->currentViewerInfo[texIndex].getDisplayWindow().getPixelAspectRatio();
            QPoint imgPosPixel;
            imgPosPixel.rx() = std::floor(imgPos.x() / par);
            imgPosPixel.ry() = std::floor(imgPos.y());
            _imp->infoViewer[texIndex]->setMousePos(imgPosPixel);
        }
    } else {
        if ( _imp->infoViewer[texIndex]->colorAndMouseVisible() ) {
            _imp->infoViewer[texIndex]->hideColorAndMouseInfo();
        }
    }
}

void
ViewerGL::updateRectangleColorPicker()
{
    float r,g,b,a;
    bool linear = appPTR->getCurrentSettings()->getColorPickerLinear();
    QPointF topLeft = _imp->pickerRect.topLeft();
    QPointF btmRight = _imp->pickerRect.bottomRight();
    RectD rect;

    rect.set_left( std::min( topLeft.x(), btmRight.x() ) );
    rect.set_right( std::max( topLeft.x(), btmRight.x() ) );
    rect.set_bottom( std::min( topLeft.y(), btmRight.y() ) );
    rect.set_top( std::max( topLeft.y(), btmRight.y() ) );
    for (int i = 0; i < 2; ++i) {
        unsigned int mm;
        bool picked = getColorAtRect(rect, linear, i, &r, &g, &b, &a,&mm);
        if (picked) {
            if (i == 0) {
                QColor pickerColor;
                pickerColor.setRedF( clamp(r) );
                pickerColor.setGreenF( clamp(g) );
                pickerColor.setBlueF( clamp(b) );
                pickerColor.setAlphaF( clamp(a) );
                _imp->viewerTab->getGui()->setColorPickersColor(pickerColor);
            }
            _imp->infoViewer[i]->setColorValid(true);
            if ( !_imp->infoViewer[i]->colorAndMouseVisible() ) {
                _imp->infoViewer[i]->showColorAndMouseInfo();
            }
            _imp->infoViewer[i]->setColorApproximated(mm > 0);
            _imp->infoViewer[i]->setColor(r, g, b, a);
        } else {
            _imp->infoViewer[i]->setColorValid(false);
        }
    }
}

void
ViewerGL::resetWipeControls()
{
    RectD rod;

    if (_imp->activeTextures[1]) {
        rod = getRoD(1);
    } else if (_imp->activeTextures[0]) {
        rod = getRoD(0);
    } else {
        _imp->getProjectFormatCanonical(rod);
    }
    {
        QMutexLocker l(&_imp->wipeControlsMutex);
        _imp->wipeCenter.setX(rod.width() / 2.);
        _imp->wipeCenter.setY(rod.height() / 2.);
        _imp->wipeAngle = 0;
        _imp->mixAmount = 1.;
    }
}

bool
ViewerGL::Implementation::isNearbyWipeCenter(const QPointF & pos,
                                             double zoomScreenPixelWidth, double zoomScreenPixelHeight) const
{
    double toleranceX = zoomScreenPixelWidth * 8.;
    double toleranceY = zoomScreenPixelHeight * 8.;
    QMutexLocker l(&wipeControlsMutex);

    if ( ( pos.x() >= (wipeCenter.x() - toleranceX) ) && ( pos.x() <= (wipeCenter.x() + toleranceX) ) &&
         ( pos.y() >= (wipeCenter.y() - toleranceY) ) && ( pos.y() <= (wipeCenter.y() + toleranceY) ) ) {
        return true;
    }

    return false;
}

bool
ViewerGL::Implementation::isNearbyWipeRotateBar(const QPointF & pos,
                                                double zoomScreenPixelWidth, double zoomScreenPixelHeight) const
{
    double toleranceX = zoomScreenPixelWidth * 8.;
    double toleranceY = zoomScreenPixelHeight * 8.;

    
    double rotateX,rotateY,rotateOffsetX,rotateOffsetY;
   
    rotateX = WIPE_ROTATE_HANDLE_LENGTH * zoomScreenPixelWidth;
    rotateY = WIPE_ROTATE_HANDLE_LENGTH * zoomScreenPixelHeight;
    rotateOffsetX = WIPE_ROTATE_OFFSET * zoomScreenPixelWidth;
    rotateOffsetY = WIPE_ROTATE_OFFSET * zoomScreenPixelHeight;
    
    QMutexLocker l(&wipeControlsMutex);
    QPointF outterPoint;

    outterPoint.setX( wipeCenter.x() + std::cos(wipeAngle) * (rotateX - rotateOffsetX) );
    outterPoint.setY( wipeCenter.y() + std::sin(wipeAngle) * (rotateY - rotateOffsetY) );
    if ( ( ( ( pos.y() >= (wipeCenter.y() - toleranceY) ) && ( pos.y() <= (outterPoint.y() + toleranceY) ) ) ||
           ( ( pos.y() >= (outterPoint.y() - toleranceY) ) && ( pos.y() <= (wipeCenter.y() + toleranceY) ) ) ) &&
         ( ( ( pos.x() >= (wipeCenter.x() - toleranceX) ) && ( pos.x() <= (outterPoint.x() + toleranceX) ) ) ||
           ( ( pos.x() >= (outterPoint.x() - toleranceX) ) && ( pos.x() <= (wipeCenter.x() + toleranceX) ) ) ) ) {
        Point a;
        a.x = ( outterPoint.x() - wipeCenter.x() );
        a.y = ( outterPoint.y() - wipeCenter.y() );
        double norm = sqrt(a.x * a.x + a.y * a.y);

        ///The point is in the bounding box of the segment, if it is vertical it must be on the segment anyway
        if (norm == 0) {
            return false;
        }

        a.x /= norm;
        a.y /= norm;
        Point b;
        b.x = ( pos.x() - wipeCenter.x() );
        b.y = ( pos.y() - wipeCenter.y() );
        norm = sqrt(b.x * b.x + b.y * b.y);

        ///This vector is not vertical
        if (norm != 0) {
            b.x /= norm;
            b.y /= norm;

            double crossProduct = b.y * a.x - b.x * a.y;
            if (std::abs(crossProduct) <  0.1) {
                return true;
            }
        }
    }

    return false;
}

bool
ViewerGL::Implementation::isNearbyWipeMixHandle(const QPointF & pos,
                                                double zoomScreenPixelWidth, double zoomScreenPixelHeight) const
{
    double toleranceX = zoomScreenPixelWidth * 8.;
    double toleranceY = zoomScreenPixelHeight * 8.;
    
    QMutexLocker l(&wipeControlsMutex);
    ///mix 1 is at rotation bar + pi / 8
    ///mix 0 is at rotation bar + 3pi / 8
    double alphaMix1,alphaMix0,alphaCurMix;
    double mpi8 = M_PI / 8;

    alphaMix1 = wipeAngle + mpi8;
    alphaMix0 = wipeAngle + 3. * mpi8;
    alphaCurMix = mixAmount * (alphaMix1 - alphaMix0) + alphaMix0;
    QPointF mixPos;
    double mixX = WIPE_MIX_HANDLE_LENGTH * zoomScreenPixelWidth;
    double mixY = WIPE_MIX_HANDLE_LENGTH * zoomScreenPixelHeight;

    mixPos.setX(wipeCenter.x() + std::cos(alphaCurMix) * mixX);
    mixPos.setY(wipeCenter.y() + std::sin(alphaCurMix) * mixY);
    if ( ( pos.x() >= (mixPos.x() - toleranceX) ) && ( pos.x() <= (mixPos.x() + toleranceX) ) &&
         ( pos.y() >= (mixPos.y() - toleranceY) ) && ( pos.y() <= (mixPos.y() + toleranceY) ) ) {
        return true;
    }

    return false;
}

bool
ViewerGL::isWipeHandleVisible() const
{
    return _imp->viewerTab->getCompositingOperator() != eViewerCompositingOperatorNone;
}

void
ViewerGL::setZoomOrPannedSinceLastFit(bool enabled)
{
    QMutexLocker l(&_imp->zoomCtxMutex);

    _imp->zoomOrPannedSinceLastFit = enabled;
}

bool
ViewerGL::getZoomOrPannedSinceLastFit() const
{
    QMutexLocker l(&_imp->zoomCtxMutex);

    return _imp->zoomOrPannedSinceLastFit;
}

Natron::ViewerCompositingOperatorEnum
ViewerGL::getCompositingOperator() const
{
    return _imp->viewerTab->getCompositingOperator();
}


void
ViewerGL::getTextureColorAt(int x,
                            int y,
                            double* r,
                            double *g,
                            double *b,
                            double *a)
{
    assert( QThread::currentThread() == qApp->thread() );
    makeCurrent();

    *r = 0;
    *g = 0;
    *b = 0;
    *a = 0;

    Texture::DataTypeEnum type;
    if (_imp->displayTextures[0]) {
        type = _imp->displayTextures[0]->type();
    } else if (_imp->displayTextures[1]) {
        type = _imp->displayTextures[1]->type();
    } else {
        return;
    }

    QPointF pos;
    {
        QMutexLocker k(&_imp->zoomCtxMutex);
        pos = _imp->zoomCtx.toWidgetCoordinates(x, y);
    }

    if ( (type == Texture::eDataTypeByte) || !_imp->supportsGLSL ) {
        U32 pixel;
        glReadBuffer(GL_FRONT);
        glReadPixels(pos.x(), height() - pos.y(), 1, 1, GL_BGRA, GL_UNSIGNED_INT_8_8_8_8_REV, &pixel);
        U8 red = 0, green = 0, blue = 0, alpha = 0;
        blue |= pixel;
        green |= (pixel >> 8);
        red |= (pixel >> 16);
        alpha |= (pixel >> 24);
        *r = (double)red / 255.;
        *g = (double)green / 255.;
        *b = (double)blue / 255.;
        *a = (double)alpha / 255.;
        glCheckError();
    } else if ( (type == Texture::eDataTypeFloat) && _imp->supportsGLSL ) {
        GLfloat pixel[4];
        glReadPixels(pos.x(), height() - pos.y(), 1, 1, GL_RGBA, GL_FLOAT, pixel);
        *r = (double)pixel[0];
        *g = (double)pixel[1];
        *b = (double)pixel[2];
        *a = (double)pixel[3];
        glCheckError();
    }
}

void
ViewerGL::Implementation::refreshSelectionRectangle(const QPointF & pos)
{
    double xmin = std::min( pos.x(),lastDragStartPos.x() );
    double xmax = std::max( pos.x(),lastDragStartPos.x() );
    double ymin = std::min( pos.y(),lastDragStartPos.y() );
    double ymax = std::max( pos.y(),lastDragStartPos.y() );

    selectionRectangle.setRect(xmin,ymin,xmax - xmin,ymax - ymin);
}

void
ViewerGL::getSelectionRectangle(double &left,
                                double &right,
                                double &bottom,
                                double &top) const
{
    QPointF topLeft = _imp->selectionRectangle.topLeft();
    QPointF btmRight = _imp->selectionRectangle.bottomRight();

    left = std::min( topLeft.x(), btmRight.x() );
    right = std::max( topLeft.x(), btmRight.x() );
    bottom = std::min( topLeft.y(), btmRight.y() );
    top = std::max( topLeft.y(), btmRight.y() );
}

boost::shared_ptr<TimeLine>
ViewerGL::getTimeline() const
{
    return _imp->viewerTab->getTimeLine();
}

void
ViewerGL::onCheckerboardSettingsChanged()
{
    _imp->initializeCheckerboardTexture(false);
    update();
}

// used by the RAII class OGLContextSaver
void
ViewerGL::saveOpenGLContext()
{
    assert(QThread::currentThread() == qApp->thread());

    glGetIntegerv(GL_TEXTURE_BINDING_2D, (GLint*)&_imp->savedTexture);
    //glGetIntegerv(GL_ACTIVE_TEXTURE, (GLint*)&_imp->activeTexture);
    glPushAttrib(GL_ALL_ATTRIB_BITS);
    glPushClientAttrib(GL_ALL_ATTRIB_BITS);
    glMatrixMode(GL_PROJECTION);
    glPushMatrix();
    glMatrixMode(GL_MODELVIEW);
    glPushMatrix();

    // set defaults to work around OFX plugin bugs
    glEnable(GL_BLEND); // or TuttleHistogramKeyer doesn't work - maybe other OFX plugins rely on this
    //glEnable(GL_TEXTURE_2D);					//Activate texturing
    //glActiveTexture (GL_TEXTURE0);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA); // or TuttleHistogramKeyer doesn't work - maybe other OFX plugins rely on this
    //glTexEnvi(GL_TEXTURE_ENV, GL_TEXTURE_ENV_MODE, GL_MODULATE); // GL_MODULATE is the default, set it
}

// used by the RAII class OGLContextSaver
void
ViewerGL::restoreOpenGLContext()
{
    assert(QThread::currentThread() == qApp->thread());

    glBindTexture(GL_TEXTURE_2D, _imp->savedTexture);
    //glActiveTexture(_imp->activeTexture);
    glMatrixMode(GL_PROJECTION);
    glPopMatrix();
    glMatrixMode(GL_MODELVIEW);
    glPopMatrix();
    glPopClientAttrib();
    glPopAttrib();
}

void
ViewerGL::clearLastRenderedTexture()
{
    {
        QMutexLocker l(&_imp->lastRenderedImageMutex);
        U64 toUnRegister = 0;
        for (int i = 0; i < 2; ++i) {
            for (U32 j = 0; j < _imp->lastRenderedImage[i].size(); ++j) {
                _imp->lastRenderedImage[i][j].reset();
            }
            toUnRegister += _imp->memoryHeldByLastRenderedImages[i];
        }
        if (toUnRegister > 0) {
            getInternalNode()->unregisterPluginMemory(toUnRegister);
        }
        
    }
}


boost::shared_ptr<Natron::Image>
ViewerGL::getLastRenderedImage(int textureIndex) const
{
    // always running in the main thread
    assert( qApp && qApp->thread() == QThread::currentThread() );
    
    if ( !getInternalNode()->getNode()->isActivated() ) {
        return boost::shared_ptr<Natron::Image>();
    }
    QMutexLocker l(&_imp->lastRenderedImageMutex);
    for (U32 i = 0; i < _imp->lastRenderedImage[textureIndex].size(); ++i) {
        if (_imp->lastRenderedImage[textureIndex][i]) {
            return _imp->lastRenderedImage[textureIndex][i];
        }
    }
    return boost::shared_ptr<Natron::Image>();
}

boost::shared_ptr<Natron::Image>
ViewerGL::getLastRenderedImageByMipMapLevel(int textureIndex,unsigned int mipMapLevel) const
{
    // always running in the main thread
    assert( qApp && qApp->thread() == QThread::currentThread() );
    
    if ( !getInternalNode()->getNode()->isActivated() ) {
        return boost::shared_ptr<Natron::Image>();
    }
    
    QMutexLocker l(&_imp->lastRenderedImageMutex);
    assert(_imp->lastRenderedImage[textureIndex].size() > mipMapLevel);
    
    if (_imp->lastRenderedImage[textureIndex][mipMapLevel]) {
        return _imp->lastRenderedImage[textureIndex][mipMapLevel];
    }
    
    //Find an image at higher scale
    if (mipMapLevel > 0) {
        for (int i = (int)mipMapLevel - 1; i >= 0; --i) {
            if (_imp->lastRenderedImage[textureIndex][i]) {
                return _imp->lastRenderedImage[textureIndex][i];
            }
        }
    }
    
    //Find an image at lower scale
    for (U32 i = mipMapLevel + 1; i < _imp->lastRenderedImage[textureIndex].size(); ++i) {
        if (_imp->lastRenderedImage[textureIndex][i]) {
            return _imp->lastRenderedImage[textureIndex][i];
        }
    }
    
    return boost::shared_ptr<Natron::Image>();

}

#ifndef M_LN2
#define M_LN2       0.693147180559945309417232121458176568  /* loge(2)        */
#endif
int
ViewerGL::getMipMapLevelCombinedToZoomFactor() const
{
    int mmLvl = getInternalNode()->getMipMapLevel();
    
    double factor = getZoomFactor();
    if (factor > 1) {
        factor = 1;
    }
    mmLvl = std::max( (double)mmLvl,-std::ceil(std::log(factor) / M_LN2) );
    
    return mmLvl;
}


unsigned int
ViewerGL::getCurrentRenderScale() const
{
    return getMipMapLevelCombinedToZoomFactor();
}

template <typename PIX,int maxValue>
static
bool
getColorAtInternal(Natron::Image* image,
                   int x,
                   int y,             // in pixel coordinates
                   bool forceLinear,
                   const Natron::Color::Lut* srcColorSpace,
                   const Natron::Color::Lut* dstColorSpace,
                   float* r,
                   float* g,
                   float* b,
                   float* a)
{
    const PIX* pix = (const PIX*)image->pixelAt(x, y);
    
    if (!pix) {
        return false;
    }
    
    Natron::ImageComponentsEnum comps = image->getComponents();
    switch (comps) {
        case Natron::eImageComponentRGBA:
            *r = pix[0] / (float)maxValue;
            *g = pix[1] / (float)maxValue;
            *b = pix[2] / (float)maxValue;
            *a = pix[3] / (float)maxValue;
            break;
        case Natron::eImageComponentRGB:
            *r = pix[0] / (float)maxValue;
            *g = pix[1] / (float)maxValue;
            *b = pix[2] / (float)maxValue;
            *a = 1.;
            break;
        case Natron::eImageComponentAlpha:
            *r = 0.;
            *g = 0.;
            *b = 0.;
            *a = pix[0] / (float)maxValue;
            break;
        default:
            assert(false);
            break;
    }
    
    
    ///convert to linear
    if (srcColorSpace) {
        *r = srcColorSpace->fromColorSpaceFloatToLinearFloat(*r);
        *g = srcColorSpace->fromColorSpaceFloatToLinearFloat(*g);
        *b = srcColorSpace->fromColorSpaceFloatToLinearFloat(*b);
    }
    
    if (!forceLinear && dstColorSpace) {
        ///convert to dst color space
        float from[3];
        from[0] = *r;
        from[1] = *g;
        from[2] = *b;
        float to[3];
        dstColorSpace->to_float_planar(to, from, 3);
        *r = to[0];
        *g = to[1];
        *b = to[2];
    }
    
    return true;
} // getColorAtInternal

bool
ViewerGL::getColorAt(double x,
                     double y,           // x and y in canonical coordinates
                     bool forceLinear,
                     int textureIndex,
                     float* r,
                     float* g,
                     float* b,
                     float* a,
                     unsigned int* imgMmlevel)                               // output values
{
    // always running in the main thread
    assert( qApp && qApp->thread() == QThread::currentThread() );
    assert(r && g && b && a);
    assert(textureIndex == 0 || textureIndex == 1);
    
    unsigned int mipMapLevel = (unsigned int)getMipMapLevelCombinedToZoomFactor();
    boost::shared_ptr<Image> img = getLastRenderedImageByMipMapLevel(textureIndex,mipMapLevel);

    
    if (!img) {
        return false;
        ///Don't do this as this is 8bit data
        /*double colorGPU[4];
        getTextureColorAt(x, y, &colorGPU[0], &colorGPU[1], &colorGPU[2], &colorGPU[3]);
        *a = colorGPU[3];
        if ( forceLinear && (_imp->displayingImageLut != eViewerColorSpaceLinear) ) {
            const Natron::Color::Lut* srcColorSpace = ViewerInstance::lutFromColorspace(_imp->displayingImageLut);
            
            *r = srcColorSpace->fromColorSpaceFloatToLinearFloat(colorGPU[0]);
            *g = srcColorSpace->fromColorSpaceFloatToLinearFloat(colorGPU[1]);
            *b = srcColorSpace->fromColorSpaceFloatToLinearFloat(colorGPU[2]);
        } else {
            *r = colorGPU[0];
            *g = colorGPU[1];
            *b = colorGPU[2];
        }
        return true;*/
    }
    
    Natron::ImageBitDepthEnum depth = img->getBitDepth();
    ViewerColorSpaceEnum srcCS = _imp->viewerTab->getGui()->getApp()->getDefaultColorSpaceForBitDepth(depth);
    const Natron::Color::Lut* dstColorSpace;
    const Natron::Color::Lut* srcColorSpace;
    if ( (srcCS == _imp->displayingImageLut) && ( (_imp->displayingImageLut == eViewerColorSpaceLinear) || !forceLinear ) ) {
        // identity transform
        srcColorSpace = 0;
        dstColorSpace = 0;
    } else {
        srcColorSpace = ViewerInstance::lutFromColorspace(srcCS);
        dstColorSpace = ViewerInstance::lutFromColorspace(_imp->displayingImageLut);
    }
    
    const double par = img->getPixelAspectRatio();
    
    double scale = 1. / (1 << img->getMipMapLevel());
    
    ///Convert to pixel coords
    int xPixel = std::floor(x  * scale / par);
    int yPixel = std::floor(y * scale);
    bool gotval;
    switch (depth) {
        case eImageBitDepthByte:
            gotval = getColorAtInternal<unsigned char, 255>(img.get(),
                                                            xPixel, yPixel,
                                                            forceLinear,
                                                            srcColorSpace,
                                                            dstColorSpace,
                                                            r, g, b, a);
            break;
        case eImageBitDepthShort:
            gotval = getColorAtInternal<unsigned short, 65535>(img.get(),
                                                               xPixel, yPixel,
                                                               forceLinear,
                                                               srcColorSpace,
                                                               dstColorSpace,
                                                               r, g, b, a);
            break;
        case eImageBitDepthFloat:
            gotval = getColorAtInternal<float, 1>(img.get(),
                                                  xPixel, yPixel,
                                                  forceLinear,
                                                  srcColorSpace,
                                                  dstColorSpace,
                                                  r, g, b, a);
            break;
        default:
            gotval = false;
            break;
    }
    *imgMmlevel = img->getMipMapLevel();
    return gotval;
} // getColorAt

bool
ViewerGL::getColorAtRect(const RectD &rect, // rectangle in canonical coordinates
                               bool forceLinear,
                               int textureIndex,
                               float* r,
                               float* g,
                               float* b,
                               float* a,
                              unsigned int* imgMm)
{
    // always running in the main thread
    assert( qApp && qApp->thread() == QThread::currentThread() );
    assert(r && g && b && a);
    assert(textureIndex == 0 || textureIndex == 1);
    
    unsigned int mipMapLevel = (unsigned int)getMipMapLevelCombinedToZoomFactor();

    boost::shared_ptr<Image> img = getLastRenderedImageByMipMapLevel(textureIndex, mipMapLevel);
  
    if (img) {
        mipMapLevel = img->getMipMapLevel();
    }
    
    ///Convert to pixel coords
    RectI rectPixel;
    rectPixel.set_left(  int( std::floor( rect.left() ) ) >> mipMapLevel);
    rectPixel.set_right( int( std::floor( rect.right() ) ) >> mipMapLevel);
    rectPixel.set_bottom(int( std::floor( rect.bottom() ) ) >> mipMapLevel);
    rectPixel.set_top(   int( std::floor( rect.top() ) ) >> mipMapLevel);
    assert( rect.bottom() <= rect.top() && rect.left() <= rect.right() );
    assert( rectPixel.bottom() <= rectPixel.top() && rectPixel.left() <= rectPixel.right() );
    double rSum = 0.;
    double gSum = 0.;
    double bSum = 0.;
    double aSum = 0.;
    if ( !img ) {
        return false;
        //don't do this as this is 8 bit
        /*
        Texture::DataTypeEnum type;
        if (_imp->displayTextures[0]) {
            type = _imp->displayTextures[0]->type();
        } else if (_imp->displayTextures[1]) {
            type = _imp->displayTextures[1]->type();
        } else {
            return false;
        }

        if ( (type == Texture::eDataTypeByte) || !_imp->supportsGLSL ) {
            std::vector<U32> pixels(rectPixel.width() * rectPixel.height());
            glReadBuffer(GL_FRONT);
            glReadPixels(rectPixel.left(), rectPixel.right(), rectPixel.width(), rectPixel.height(),
                         GL_BGRA, GL_UNSIGNED_INT_8_8_8_8_REV, &pixels.front());
            double rF,gF,bF,aF;
            for (U32 i = 0 ; i < pixels.size(); ++i) {
                U8 red = 0, green = 0, blue = 0, alpha = 0;
                blue |= pixels[i];
                green |= (pixels[i] >> 8);
                red |= (pixels[i] >> 16);
                alpha |= (pixels[i] >> 24);
                rF = (double)red / 255.;
                gF = (double)green / 255.;
                bF = (double)blue / 255.;
                aF = (double)alpha / 255.;
                
                aSum += aF;
                if ( forceLinear && (_imp->displayingImageLut != eViewerColorSpaceLinear) ) {
                    const Natron::Color::Lut* srcColorSpace = ViewerInstance::lutFromColorspace(_imp->displayingImageLut);
                    
                    rSum += srcColorSpace->fromColorSpaceFloatToLinearFloat(rF);
                    gSum += srcColorSpace->fromColorSpaceFloatToLinearFloat(gF);
                    bSum += srcColorSpace->fromColorSpaceFloatToLinearFloat(bF);
                } else {
                    rSum += rF;
                    gSum += gF;
                    bSum += bF;
                }

            }
            
            glCheckError();
        } else if ( (type == Texture::eDataTypeFloat) && _imp->supportsGLSL ) {
            std::vector<float> pixels(rectPixel.width() * rectPixel.height() * 4);
            glReadPixels(rectPixel.left(), rectPixel.right(), rectPixel.width(), rectPixel.height(),
                         GL_RGBA, GL_FLOAT, &pixels.front());
            
            int rowSize = rectPixel.width() * 4;
            for (int y = 0; y < rectPixel.height(); ++y) {
                for (int x = 0; x < rectPixel.width(); ++x) {
                    double rF = pixels[y * rowSize + (4 * x)];
                    double gF = pixels[y * rowSize + (4 * x) + 1];
                    double bF = pixels[y * rowSize + (4 * x) + 2];
                    double aF = pixels[y * rowSize + (4 * x) + 3];
                    
                    aSum += aF;
                    if ( forceLinear && (_imp->displayingImageLut != eViewerColorSpaceLinear) ) {
                        const Natron::Color::Lut* srcColorSpace = ViewerInstance::lutFromColorspace(_imp->displayingImageLut);
                        
                        rSum += srcColorSpace->fromColorSpaceFloatToLinearFloat(rF);
                        gSum += srcColorSpace->fromColorSpaceFloatToLinearFloat(gF);
                        bSum += srcColorSpace->fromColorSpaceFloatToLinearFloat(bF);
                    } else {
                        rSum += rF;
                        gSum += gF;
                        bSum += bF;
                    }
                }
            }
          

            glCheckError();
        }
 
        *r = rSum / rectPixel.area();
        *g = gSum / rectPixel.area();
        *b = bSum / rectPixel.area();
        *a = aSum / rectPixel.area();
        
        return true;*/
    }
    
    
    Natron::ImageBitDepthEnum depth = img->getBitDepth();
    ViewerColorSpaceEnum srcCS = _imp->viewerTab->getGui()->getApp()->getDefaultColorSpaceForBitDepth(depth);
    const Natron::Color::Lut* dstColorSpace;
    const Natron::Color::Lut* srcColorSpace;
    if ( (srcCS == _imp->displayingImageLut) && ( (_imp->displayingImageLut == eViewerColorSpaceLinear) || !forceLinear ) ) {
        // identity transform
        srcColorSpace = 0;
        dstColorSpace = 0;
    } else {
        srcColorSpace = ViewerInstance::lutFromColorspace(srcCS);
        dstColorSpace = ViewerInstance::lutFromColorspace(_imp->displayingImageLut);
    }
    
    unsigned long area = 0;
    for (int yPixel = rectPixel.bottom(); yPixel < rectPixel.top(); ++yPixel) {
        for (int xPixel = rectPixel.left(); xPixel < rectPixel.right(); ++xPixel) {
            float rPix, gPix, bPix, aPix;
            bool gotval = false;
            switch (depth) {
                case eImageBitDepthByte:
                    gotval = getColorAtInternal<unsigned char, 255>(img.get(),
                                                                    xPixel, yPixel,
                                                                    forceLinear,
                                                                    srcColorSpace,
                                                                    dstColorSpace,
                                                                    &rPix, &gPix, &bPix, &aPix);
                    break;
                case eImageBitDepthShort:
                    gotval = getColorAtInternal<unsigned short, 65535>(img.get(),
                                                                       xPixel, yPixel,
                                                                       forceLinear,
                                                                       srcColorSpace,
                                                                       dstColorSpace,
                                                                       &rPix, &gPix, &bPix, &aPix);
                    break;
                case eImageBitDepthFloat:
                    gotval = getColorAtInternal<float, 1>(img.get(),
                                                          xPixel, yPixel,
                                                          forceLinear,
                                                          srcColorSpace,
                                                          dstColorSpace,
                                                          &rPix, &gPix, &bPix, &aPix);
                    break;
                case eImageBitDepthNone:
                    break;
            }
            if (gotval) {
                rSum += rPix;
                gSum += gPix;
                bSum += bPix;
                aSum += aPix;
                ++area;
            }
        }
    }
    
    *imgMm = img->getMipMapLevel();
    
    if (area > 0) {
        *r = rSum / area;
        *g = gSum / area;
        *b = bSum / area;
        *a = aSum / area;
        
        return true;
    }
    
    return false;
} // getColorAtRect


int
ViewerGL::getCurrentlyDisplayedTime() const
{
    QMutexLocker k(&_imp->lastRenderedImageMutex);
    if (_imp->activeTextures[0]) {
        return _imp->displayingImageTime[0];
    } else {
        return _imp->viewerTab->getTimeLine()->currentFrame();
    }
}

void
ViewerGL::getViewerFrameRange(int* first,int* last) const
{
    _imp->viewerTab->getTimelineBounds(first, last);
}

