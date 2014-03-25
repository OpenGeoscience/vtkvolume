﻿#include "vtkSinglePassVolumeMapper.h"

#include "GLSLShader.h"
#include "vtkOpenGLOpacityTable.h"
#include "vtkOpenGLVolumeRGBTable.h"

#include <vtkBoundingBox.h>
#include <vtkCamera.h>
#include <vtkColorTransferFunction.h>
#include <vtkFloatArray.h>
#include <vtkImageData.h>
#include <vtkMatrix4x4.h>
#include <vtkObjectFactory.h>
#include <vtkPointData.h>
#include <vtkPoints.h>
#include <vtkRenderer.h>
#include <vtkRenderWindow.h>
#include <vtkSmartPointer.h>
#include <vtkUnsignedCharArray.h>
#include <vtkVolumeProperty.h>

#include <GL/glew.h>
#include <vtkgl.h>

#include <cassert>

vtkStandardNewMacro(vtkSinglePassVolumeMapper);

/// TODO Remove this afterwards
#define GL_CHECK_ERRORS \
  {\
  std::cerr << "Checking for error " << glGetError() << std::endl; \
  assert(glGetError()== GL_NO_ERROR); \
  }

//--------------------------------------------------------------------------
///
/// \brief The vtkSinglePassVolumeMapper::vtkInternal class
///
class vtkSinglePassVolumeMapper::vtkInternal
{
public:
  ///
  /// \brief vtkInternal
  /// \param parent
  ///
  vtkInternal(vtkSinglePassVolumeMapper* parent) :
    Initialized(false),
    ValidTransferFunction(false),
    CellFlag(-1),
    TextureWidth(1024),
    Parent(parent),
    RGBTable(0)
    {
    this->TextureSize[0] = this->TextureSize[1] = this->TextureSize[2] = -1;
    this->SampleDistance[0] = this->SampleDistance[1] =
      this->SampleDistance[2] = 0.0;
    this->CellScale[0] = this->CellScale[1] = this->CellScale[2] = 0.0;

    // TODO Initialize extents and scalars range
    }

  ~vtkInternal()
    {
    delete this->RGBTable;
    this->RGBTable = 0;

    delete this->OpacityTables;
    this->OpacityTables = 0;
    }

  //--------------------------------------------------------------------------
  ///
  /// \brief Initialize
  ///
  void Initialize()
    {
    GLenum err = glewInit();
    if (GLEW_OK != err)	{
        cerr <<"Error: "<< glewGetErrorString(err)<<endl;
    } else {
        if (GLEW_VERSION_3_3)
        {
            cout<<"Driver supports OpenGL 3.3\nDetails:"<<endl;
        }
    }
    /// This is to ignore INVALID ENUM error 1282
    err = glGetError();
    GL_CHECK_ERRORS

    //output hardware information
    cout<<"\tUsing GLEW "<< glewGetString(GLEW_VERSION)<<endl;
    cout<<"\tVendor: "<< glGetString (GL_VENDOR)<<endl;
    cout<<"\tRenderer: "<< glGetString (GL_RENDERER)<<endl;
    cout<<"\tVersion: "<< glGetString (GL_VERSION)<<endl;
    cout<<"\tGLSL: "<< glGetString (GL_SHADING_LANGUAGE_VERSION)<<endl;

    /// Load the raycasting shader
    this->Shader.LoadFromFile(GL_VERTEX_SHADER, "shaders/raycaster.vert");
    this->Shader.LoadFromFile(GL_FRAGMENT_SHADER, "shaders/raycaster.frag");

    /// Compile and link the shader
    this->Shader.CreateAndLinkProgram();
    this->Shader.Use();

    /// Add attributes and uniforms
    this->Shader.AddAttribute("in_vertex_pos");
    this->Shader.AddUniform("modelview_matrix");
    this->Shader.AddUniform("projection_matrix");
    this->Shader.AddUniform("volume");
    this->Shader.AddUniform("camera_pos");
    this->Shader.AddUniform("step_size");
    this->Shader.AddUniform("cell_scale");
    this->Shader.AddUniform("color_transfer_func");
    this->Shader.AddUniform("opacity_transfer_func");
    this->Shader.AddUniform("vol_extents_min");
    this->Shader.AddUniform("vol_extents_max");
    this->Shader.AddUniform("enable_shading");

    // Setup unit cube vertex array and vertex buffer objects
    glGenVertexArrays(1, &this->CubeVAOId);
    glGenBuffers(1, &this->CubeVBOId);
    glGenBuffers(1, &this->CubeIndicesId);
    glGenBuffers(1, &this->CubeTextureId);

    this->RGBTable = new vtkOpenGLVolumeRGBTable();

    /// TODO Currently we are supporting only one level
    this->OpacityTables = new vtkOpenGLOpacityTables(1);

    this->Shader.UnUse();

    this->Initialized = true;
    }

  //--------------------------------------------------------------------------
  ///
  /// \brief GetVolumeTexture
  /// \return Volume texture ID
  ///
  unsigned int GetVolumeTexture()
    {
    return this->TextureId;
    }

  //--------------------------------------------------------------------------
  ///
  /// \brief UpdateColorTransferFunction
  /// \param vol
  /// \param numberOfScalarComponents
  /// \return 0 (passed) or 1 (failed)
  ///
  /// Update transfer color function based on the incoming inputs and number of
  /// scalar components.
  ///
  /// TODO Deal with numberOfScalarComponents > 1
  int UpdateColorTransferFunction(vtkVolume* vol, int numberOfScalarComponents)
  {
    /// Build the colormap in a 1D texture.
    /// 1D RGB-texture=mapping from scalar values to color values
    /// build the table.
    if(numberOfScalarComponents == 1)
      {
      vtkVolumeProperty* volumeProperty = vol->GetProperty();
      vtkColorTransferFunction* colorTransferFunction =
        volumeProperty->GetRGBTransferFunction(0);

      colorTransferFunction->AddRGBPoint(this->ScalarsRange[0], 0.0, 0.0, 0.0);
      colorTransferFunction->AddRGBPoint(this->ScalarsRange[1], 1.0, 1.0, 1.0);

      /// Activate texture 1
      glActiveTexture(GL_TEXTURE1);

      this->RGBTable->Update(
        colorTransferFunction, this->ScalarsRange,
        volumeProperty->GetInterpolationType() == VTK_LINEAR_INTERPOLATION);

      glActiveTexture(GL_TEXTURE0);
      }
    else
      {
      std::cerr << "SinglePass volume mapper does not handle multi-component scalars";
      return 1;
      }

    return 0;
  }

  //--------------------------------------------------------------------------
  ///
  /// \brief vtkOpenGLGPUVolumeRayCastMapper::UpdateOpacityTransferFunction
  /// \param vol
  /// \param numberOfScalarComponents (1 or 4)
  /// \param level
  /// \return 0 or 1 (fail)
  ///
  int UpdateOpacityTransferFunction(vtkVolume* vol, int numberOfScalarComponents,
                                    unsigned int level)
  {
    if (!vol)
      {
      std::cerr << "Invalid volume" << std::endl;
      return 1;
      }

    if (numberOfScalarComponents != 1)
      {
      std::cerr << "SinglePass volume mapper does not handle multi-component scalars";
      return 1;
      }

    vtkVolumeProperty* volumeProperty = vol->GetProperty();
    vtkPiecewiseFunction* scalarOpacity = volumeProperty->GetScalarOpacity();

    /// TODO: Do a better job to create the default opacity map
    scalarOpacity->AddPoint(this->ScalarsRange[0], 0.0);
    scalarOpacity->AddPoint(this->ScalarsRange[1], 1.0);

    /// Activate texture 2
    glActiveTexture(GL_TEXTURE2);

    this->OpacityTables->GetTable(level)->Update(
      scalarOpacity,this->BlendMode,
      this->SampleDistance,
      this->ScalarsRange,
      volumeProperty->GetScalarOpacityUnitDistance(0),
      volumeProperty->GetInterpolationType() == VTK_LINEAR_INTERPOLATION);

    /// Restore default active texture
    glActiveTexture(GL_TEXTURE0);

    return 0;
  }

  //--------------------------------------------------------------------------
  ///
  /// \brief LoadVolume
  /// \param imageData
  /// \param scalars
  /// \return
  ///
  bool LoadVolume(vtkImageData* imageData, vtkDataArray* scalars);

  //--------------------------------------------------------------------------
  ///
  /// \brief IsDataDirty
  /// \param imageData
  /// \return
  ///
  bool IsDataDirty(vtkImageData* imageData);

  //--------------------------------------------------------------------------
  ///
  /// \brief IsInitialized
  /// \return
  ///
  bool IsInitialized();

  //--------------------------------------------------------------------------
  ///
  /// \brief HasBoundsChanged
  /// \param bounds
  /// \return
  ///
  bool HasBoundsChanged(double* bounds);

  ///
  /// Private member variables

  bool Initialized;
  bool ValidTransferFunction;

  GLuint CubeVBOId;
  GLuint CubeVAOId;
  GLuint CubeIndicesId;
  GLuint CubeTextureId;

  GLSLShader Shader;

  GLuint TextureId;
  GLuint TransferFuncSampler;

  int CellFlag;
  int TextureSize[3];
  int TextureExtents[6];
  int TextureWidth;
  int BlendMode;

  double ScalarsRange[2];
  double Bounds[6];
  double SampleDistance[3];
  double CellScale[3];

  vtkSinglePassVolumeMapper* Parent;
  vtkOpenGLVolumeRGBTable* RGBTable;
  vtkOpenGLOpacityTables* OpacityTables;

  vtkTimeStamp VolumeBuildTime;
};

//--------------------------------------------------------------------------
///
/// \brief vtkSinglePassVolumeMapper::vtkInternal::LoadVolume
/// \param imageData
/// \param scalars
/// \return
///
bool vtkSinglePassVolumeMapper::vtkInternal::LoadVolume(vtkImageData* imageData,
                                                        vtkDataArray* scalars)
{
  /// Generate OpenGL texture
  glEnable(GL_TEXTURE_3D);
  glGenTextures(1, &this->TextureId);
  glBindTexture(GL_TEXTURE_3D, this->TextureId);

  /// Set the texture parameters
  glTexParameteri(GL_TEXTURE_3D, GL_TEXTURE_WRAP_S, GL_CLAMP);
  glTexParameteri(GL_TEXTURE_3D, GL_TEXTURE_WRAP_T, GL_CLAMP);
  glTexParameteri(GL_TEXTURE_3D, GL_TEXTURE_WRAP_R, GL_CLAMP);
  glTexParameteri(GL_TEXTURE_3D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
  glTexParameteri(GL_TEXTURE_3D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);

  // TODO Make it configurable
  // Set the mipmap levels (base and max)
  //  glTexParameteri(GL_TEXTURE_3D, GL_TEXTURE_BASE_LEVEL, 0);
  //  glTexParameteri(GL_TEXTURE_3D, GL_TEXTURE_MAX_LEVEL, 4);

  GL_CHECK_ERRORS

  /// Allocate data with internal format and foramt as (GL_RED)
  GLint internalFormat = 0;
  GLenum format = 0;
  GLenum type = 0;

  double shift = 0.0;
  double scale = 1.0;
  int needTypeConversion = 0;

  int scalarType = scalars->GetDataType();
  if(scalars->GetNumberOfComponents()==4)
    {
    internalFormat = GL_RGBA16;
    format = GL_RGBA;
    type = GL_UNSIGNED_BYTE;
    }
  else
    {
    switch(scalarType)
      {
      case VTK_FLOAT:
      if (glewIsSupported("GL_ARB_texture_float"))
        {
        internalFormat = vtkgl::INTENSITY16F_ARB;
        }
      else
        {
        internalFormat = GL_INTENSITY16;
        }
        format = GL_RED;
        type = GL_FLOAT;
        shift=-ScalarsRange[0];
        scale = 1/(this->ScalarsRange[1]-this->ScalarsRange[0]);
        break;
      case VTK_UNSIGNED_CHAR:
        internalFormat = GL_INTENSITY8;
        format = GL_RED;
        type = GL_UNSIGNED_BYTE;
        shift = -this->ScalarsRange[0]/VTK_UNSIGNED_CHAR_MAX;
        scale =
          VTK_UNSIGNED_CHAR_MAX/(this->ScalarsRange[1]-this->ScalarsRange[0]);
        break;
      case VTK_SIGNED_CHAR:
        internalFormat = GL_INTENSITY8;
        format = GL_RED;
        type = GL_BYTE;
        shift=-(2*this->ScalarsRange[0]+1)/VTK_UNSIGNED_CHAR_MAX;
        scale = VTK_SIGNED_CHAR_MAX/(this->ScalarsRange[1]-this->ScalarsRange[0]);
        break;
      case VTK_CHAR:
        // not supported
        assert("check: impossible case" && 0);
        break;
      case VTK_BIT:
        // not supported
        assert("check: impossible case" && 0);
        break;
      case VTK_ID_TYPE:
        // not supported
        assert("check: impossible case" && 0);
        break;
      case VTK_INT:
        internalFormat = GL_INTENSITY16;
        format = GL_RED;
        type = GL_INT;
        shift=-(2*this->ScalarsRange[0]+1)/VTK_UNSIGNED_INT_MAX;
        scale = VTK_INT_MAX/(this->ScalarsRange[1]-this->ScalarsRange[0]);
        break;
      case VTK_DOUBLE:
      case VTK___INT64:
      case VTK_LONG:
      case VTK_LONG_LONG:
      case VTK_UNSIGNED___INT64:
      case VTK_UNSIGNED_LONG:
      case VTK_UNSIGNED_LONG_LONG:
        /// TODO Implement support for this
        std::cerr << "Scalar type VTK_UNSIGNED_LONG_LONG not supported" << std::endl;
        break;
      case VTK_SHORT:
        internalFormat = GL_INTENSITY16;
        format = GL_RED;
        type = GL_SHORT;
        shift=-(2*this->ScalarsRange[0]+1)/VTK_UNSIGNED_SHORT_MAX;
        scale = VTK_SHORT_MAX/(this->ScalarsRange[1]-this->ScalarsRange[0]);
        break;
      case VTK_STRING:
        // not supported
        assert("check: impossible case" && 0);
        break;
      case VTK_UNSIGNED_SHORT:
        internalFormat = GL_INTENSITY16;
        format = GL_RED;
        type = GL_UNSIGNED_SHORT;

        shift=-this->ScalarsRange[0]/VTK_UNSIGNED_SHORT_MAX;
        scale=
          VTK_UNSIGNED_SHORT_MAX/(this->ScalarsRange[1]-this->ScalarsRange[0]);
        break;
      case VTK_UNSIGNED_INT:
        internalFormat = GL_INTENSITY16;
        format = GL_RED;
        type = GL_UNSIGNED_INT;
        shift=-this->ScalarsRange[0]/VTK_UNSIGNED_INT_MAX;
        scale = VTK_UNSIGNED_INT_MAX/(this->ScalarsRange[1]-this->ScalarsRange[0]);
        break;
      default:
        assert("check: impossible case" && 0);
        break;
      }
    }

  int* ext = imageData->GetExtent();
  this->TextureExtents[0] = ext[0];
  this->TextureExtents[1] = ext[1];
  this->TextureExtents[2] = ext[2];
  this->TextureExtents[3] = ext[3];
  this->TextureExtents[4] = ext[4];
  this->TextureExtents[5] = ext[5];

  void* dataPtr = scalars->GetVoidPointer(0);
  int i = 0;
  while(i < 3)
    {
    this->TextureSize[i] = this->TextureExtents[2*i+1] - this->TextureExtents[2*i] + 1;
    ++i;
    }


  glTexImage3D(GL_TEXTURE_3D, 0, internalFormat,
               this->TextureSize[0],this->TextureSize[1],this->TextureSize[2], 0,
               format, type, dataPtr);

  GL_CHECK_ERRORS


  /// TODO Enable mipmapping later
  // Generate mipmaps
  //glGenerateMipmap(GL_TEXTURE_3D);

  /// Update volume build time
  this->VolumeBuildTime.Modified();
  return 1;
}

//--------------------------------------------------------------------------
///
/// \brief vtkSinglePassVolumeMapper::vtkInternal::IsInitialized
/// \return
///
bool vtkSinglePassVolumeMapper::vtkInternal::IsInitialized()
{
  return this->Initialized;
}

//--------------------------------------------------------------------------
///
/// \brief vtkSinglePassVolumeMapper::vtkInternal::IsDataDirty
/// \param input
/// \return
///
bool vtkSinglePassVolumeMapper::vtkInternal::IsDataDirty(vtkImageData* input)
{
  /// Check if the scalars modified time is higher than the last build time
  /// if yes, then mark the current referenced data as dirty.
  if (input->GetMTime() > this->VolumeBuildTime.GetMTime())
    {
    return true;
    }

  return false;
}

//--------------------------------------------------------------------------
///
/// \brief vtkSinglePassVolumeMapper::vtkInternal::HasBoundsChanged
/// \param bounds
/// \return
///
bool vtkSinglePassVolumeMapper::vtkInternal::HasBoundsChanged(double* bounds)
{
  /// TODO Detect if the camera is inside the bbox and if yes, update the bounds
  /// accordingly so that we can zoom through it.
  if (bounds[0] == this->Bounds[0] &&
      bounds[1] == this->Bounds[1] &&
      bounds[2] == this->Bounds[2] &&
      bounds[3] == this->Bounds[3] &&
      bounds[4] == this->Bounds[4] &&
      bounds[5] == this->Bounds[5])
    {
    return false;
    }
  else
    {
    return true;
    }
}

//--------------------------------------------------------------------------
///
/// \brief vtkSinglePassVolumeMapper::vtkSinglePassVolumeMapper
///
vtkSinglePassVolumeMapper::vtkSinglePassVolumeMapper() : vtkVolumeMapper()
{
  this->Implementation = new vtkInternal(this);
}

//--------------------------------------------------------------------------
///
/// \brief vtkSinglePassVolumeMapper::~vtkSinglePassVolumeMapper
///
vtkSinglePassVolumeMapper::~vtkSinglePassVolumeMapper()
{
  delete this->Implementation;
  this->Implementation = 0;
}

//--------------------------------------------------------------------------
///
/// \brief vtkSinglePassVolumeMapper::PrintSelf
/// \param os
/// \param indent
///
void vtkSinglePassVolumeMapper::PrintSelf(ostream &os, vtkIndent indent)
{
  // TODO Implement this method
}

//--------------------------------------------------------------------------
///
/// \brief vtkSinglePassVolumeMapper::Render
/// \param ren
/// \param vol
///
void vtkSinglePassVolumeMapper::Render(vtkRenderer* ren, vtkVolume* vol)
{
  /// Make sure the context is current
  ren->GetRenderWindow()->MakeCurrent();

  vtkImageData* input = this->GetInput();
  glClear(GL_DEPTH_BUFFER_BIT);
  glClearColor(1.0, 1.0, 1.0, 1.0);

  glEnable(GL_TEXTURE_1D);
  glEnable(GL_TEXTURE_3D);

  if (!this->Implementation->IsInitialized())
    {
    this->Implementation->Initialize();
    }

  vtkDataArray* scalars = this->GetScalars(input,
                          this->ScalarMode,
                          this->ArrayAccessMode,
                          this->ArrayId,
                          this->ArrayName,
                          this->Implementation->CellFlag);

  scalars->GetRange(this->Implementation->ScalarsRange);

  /// Local variables
  double bounds[6];
  vol->GetBounds(bounds);

  /// Load volume data if needed
  if (this->Implementation->IsDataDirty(input))
    {
    this->Implementation->LoadVolume(input, scalars);
    }

  /// Use the shader
  this->Implementation->Shader.Use();

  /// Update opacity transfer function
  /// TODO Passing level 0 for now
  this->Implementation->UpdateOpacityTransferFunction(vol,
    scalars->GetNumberOfComponents(), 0);

  /// Update transfer color functions
  this->Implementation->UpdateColorTransferFunction(vol,
    scalars->GetNumberOfComponents());

  GL_CHECK_ERRORS

  if (this->Implementation->HasBoundsChanged(bounds))
    {
    /// Cube vertices
    float vertices[8][3] =
      {
      {bounds[0], bounds[2], bounds[4]}, // 0
      {bounds[1], bounds[2], bounds[4]}, // 1
      {bounds[1], bounds[3], bounds[4]}, // 2
      {bounds[0], bounds[3], bounds[4]}, // 3
      {bounds[0], bounds[2], bounds[5]}, // 4
      {bounds[1], bounds[2], bounds[5]}, // 5
      {bounds[1], bounds[3], bounds[5]}, // 6
      {bounds[0], bounds[3], bounds[5]}  // 7
      };

    /// Cube indices
    GLushort cubeIndices[36]=
      {
      0,5,4, // bottom
      5,0,1, // bottom
      3,7,6, // top
      3,6,2, // op
      7,4,6, // front
      6,4,5, // front
      2,1,3, // left side
      3,1,0, // left side
      3,0,7, // right side
      7,0,4, // right side
      6,5,2, // back
      2,5,1  // back
      };

    glBindVertexArray(this->Implementation->CubeVAOId);

    /// Pass cube vertices to buffer object memory
    glBindBuffer (GL_ARRAY_BUFFER, this->Implementation->CubeVBOId);
    glBufferData (GL_ARRAY_BUFFER, sizeof(vertices), &(vertices[0][0]), GL_STATIC_DRAW);

    GL_CHECK_ERRORS

    /// Enable vertex attributre array for position
    /// and pass indices to element array  buffer
    glEnableVertexAttribArray(this->Implementation->Shader["in_vertex_pos"]);
    glVertexAttribPointer(this->Implementation->Shader["in_vertex_pos"], 3, GL_FLOAT, GL_FALSE, 0, 0);

    glBindBuffer (GL_ELEMENT_ARRAY_BUFFER, this->Implementation->CubeIndicesId);
    glBufferData (GL_ELEMENT_ARRAY_BUFFER, sizeof(cubeIndices), &cubeIndices[0], GL_STATIC_DRAW);

    GL_CHECK_ERRORS

    glBindVertexArray(0);
    }

  /// Enable depth test
  glEnable(GL_DEPTH_TEST);

  /// Set the over blending function
  glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);

  /// Enable blending
  glEnable(GL_BLEND);

  GL_CHECK_ERRORS

  /// Update sampling distance
  this->Implementation->SampleDistance[0] = 1.0 / (bounds[1] - bounds[0]);
  this->Implementation->SampleDistance[1] = 1.0 / (bounds[3] - bounds[2]);
  this->Implementation->SampleDistance[2] = 1.0 / (bounds[5] - bounds[4]);

  this->Implementation->CellScale[0] = (bounds[1] - bounds[0]) * 0.5;
  this->Implementation->CellScale[1] = (bounds[3] - bounds[2]) * 0.5;
  this->Implementation->CellScale[2] = (bounds[5] - bounds[4]) * 0.5;

  /// Pass constant uniforms at initialization
  /// Step should be dependant on the bounds and not on the texture size
  /// since we can have non uniform voxel size / spacing / aspect ratio
  glUniform3f(this->Implementation->Shader("step_size"),
              this->Implementation->SampleDistance[0],
              this->Implementation->SampleDistance[1],
              this->Implementation->SampleDistance[2]);

  glUniform3f(this->Implementation->Shader("cell_scale"),
              this->Implementation->CellScale[0],
              this->Implementation->CellScale[1],
              this->Implementation->CellScale[2]);

  glUniform1i(this->Implementation->Shader("volume"), 0);
  glUniform1i(this->Implementation->Shader("color_transfer_func"), 1);
  glUniform1i(this->Implementation->Shader("opacity_transfer_func"), 2);

  /// Shading is ON by default
  /// TODO Add an API to enable / disable shading if not present
  glUniform1i(this->Implementation->Shader("enable_shading"), 1);

  /// Bind textures
  /// Volume texture is at unit 0
  glActiveTexture(GL_TEXTURE0);
  glBindTexture(GL_TEXTURE_3D, this->Implementation->GetVolumeTexture());

  /// Color texture is at unit 1
  glActiveTexture(GL_TEXTURE1);
  this->Implementation->RGBTable->Bind();

  /// Opacity texture is at unit 2
  /// TODO Supports only one table for now
  glActiveTexture(GL_TEXTURE2);
  this->Implementation->OpacityTables->GetTable(0)->Bind();

  /// Look at the OpenGL Camera for the exact aspect computation
  double aspect[2];
  ren->ComputeAspect();
  ren->GetAspect(aspect);

  double clippingRange[2];
  ren->GetActiveCamera()->GetClippingRange(clippingRange);

  /// Will require transpose of this matrix for OpenGL
  /// Fix this
  vtkMatrix4x4* projMat = ren->GetActiveCamera()->
    GetProjectionTransformMatrix(aspect[0]/aspect[1], 0, 1);
  float projectionMat[16];
  for (int i = 0; i < 4; ++i)
    {
    for (int j = 0; j < 4; ++j)
      {
      projectionMat[i * 4 + j] = projMat->Element[i][j];
      }
    }

  /// Will require transpose of this matrix for OpenGL
  /// Fix this
  vtkMatrix4x4* mvMat = ren->GetActiveCamera()->GetViewTransformMatrix();
  float modelviewMat[16];
  for (int i = 0; i < 4; ++i)
    {
    for (int j = 0; j < 4; ++j)
      {
      modelviewMat[i * 4 + j] = mvMat->Element[i][j];
      }
    }

  glUniformMatrix4fv(this->Implementation->Shader("projection_matrix"), 1,
                     GL_FALSE, &(projectionMat[0]));
  glUniformMatrix4fv(this->Implementation->Shader("modelview_matrix"), 1,
                     GL_FALSE, &(modelviewMat[0]));

  /// We are using float for now
  double* cameraPos = ren->GetActiveCamera()->GetPosition();
  float pos[3] = {static_cast<float>(cameraPos[0]),
                  static_cast<float>(cameraPos[1]),
                  static_cast<float>(cameraPos[2])};

  glUniform3fv(this->Implementation->Shader("camera_pos"), 1, &(pos[0]));

  float volExtentsMin[3] = {bounds[0], bounds[2], bounds[4]};
  float volExtentsMax[3] = {bounds[1], bounds[3], bounds[5]};

  glUniform3fv(this->Implementation->Shader("vol_extents_min"), 1,
               &(volExtentsMin[0]));
  glUniform3fv(this->Implementation->Shader("vol_extents_max"), 1,
               &(volExtentsMax[0]));

  glBindVertexArray(this->Implementation->CubeVAOId);
  glDrawElements(GL_TRIANGLES, 36, GL_UNSIGNED_SHORT, 0);

  /// Undo binds and state changes
  /// TODO Provide a stack implementation
  this->Implementation->Shader.UnUse();

  glBindVertexArray(0);
  glDisable(GL_BLEND);

  glDisable(GL_TEXTURE_3D);
  glDisable(GL_TEXTURE_1D);

  glActiveTexture(GL_TEXTURE0);
}