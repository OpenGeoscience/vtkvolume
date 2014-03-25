#ifndef __vtkOpenGLVolumeRGBTable_h_
#define __vtkOpenGLVolumeRGBTable_h_

#include <vtkColorTransferFunction.h>

#include <GL/glew.h>
#include <vtkgl.h>

//----------------------------------------------------------------------------
///
/// \brief The vtkOpenGLVolumeRGBTable class
///
class vtkOpenGLVolumeRGBTable
{
public:
  //--------------------------------------------------------------------------
  ///
  /// \brief Constructor.
  ///
  vtkOpenGLVolumeRGBTable()
    {
      this->Loaded = false;
      this->LastLinearInterpolation = false;
      this->TexutureWidth = 1024;
      this->NumberOfColorComponents = 3;
      this->TextureId = 0;
      this->LastRange[0] = this->LastRange[1] = 0;
      this->Table = 0;
    }

  //--------------------------------------------------------------------------
  /// \brief Destructor.
  ~vtkOpenGLVolumeRGBTable()
    {
      if(this->TextureId!=0)
        {
        glDeleteTextures(1,&this->TextureId);
        this->TextureId=0;
        }
      if(this->Table!=0)
        {
        delete[] this->Table;
        this->Table=0;
        }
    }

  //--------------------------------------------------------------------------
  ///
  /// \brief Check if color transfer function texture is loaded.
  /// \return
  ///
  bool IsLoaded()
    {
      return this->Loaded;
    }

  //--------------------------------------------------------------------------
  ///
  /// \brief Bind texture.
  ///
  void Bind()
    {
      glBindTexture(GL_TEXTURE_1D, this->TextureId);
      glDisable(GL_TEXTURE_1D);
    }

  //--------------------------------------------------------------------------
  ///
  /// \brief Update color transfer function texture.
  /// \param scalarRGB
  /// \param range
  /// \param linearInterpolation
  ///
  void Update(vtkColorTransferFunction *scalarRGB,
              double range[2],
              bool linearInterpolation)
    {
      bool needUpdate = false;

      if(this->TextureId == 0)
        {
        glGenTextures(1, &this->TextureId);
        needUpdate = true;
        }

      if (range[0] != this->LastRange[0] || range[1] != this->LastRange[1])
        {
        needUpdate=true;
        }

      glBindTexture(GL_TEXTURE_1D, this->TextureId);
      if(needUpdate)
        {
        glTexParameteri(GL_TEXTURE_1D, GL_TEXTURE_WRAP_S,
                        vtkgl::CLAMP_TO_EDGE);
        }
      if(scalarRGB->GetMTime() > this->BuildTime || needUpdate || !this->Loaded)
        {
        this->Loaded = false;

        /// Create table if not created already
        if(this->Table==0)
          {
          this->Table = new float[this->TexutureWidth * this->NumberOfColorComponents];
          }

        scalarRGB->GetTable(range[0],range[1], this->TexutureWidth, this->Table);
        glTexImage1D(GL_TEXTURE_1D,0,GL_RGB16, this->TexutureWidth, 0,
                     GL_RGB,GL_FLOAT,this->Table);

        this->Loaded = true;
        this->BuildTime.Modified();

        this->LastRange[0] = range[0];
        this->LastRange[1] = range[1];
        }

      needUpdate = needUpdate || this->LastLinearInterpolation!=linearInterpolation;
      if (needUpdate)
        {
        this->LastLinearInterpolation = linearInterpolation;
        GLint value;
        if (linearInterpolation)
          {
          value = GL_LINEAR;
          }
        else
          {
          value = GL_NEAREST;
          }
        glTexParameteri(GL_TEXTURE_1D,GL_TEXTURE_MIN_FILTER,value);
        glTexParameteri(GL_TEXTURE_1D,GL_TEXTURE_MAG_FILTER,value);
        }
    }

protected:

  bool Loaded;
  bool LastLinearInterpolation;

  int TexutureWidth;
  int NumberOfColorComponents;

  GLuint TextureId;

  double LastRange[2];
  float* Table;
  vtkTimeStamp BuildTime;
};

#endif // __vtkOpenGLVolumeRGBTable_h_