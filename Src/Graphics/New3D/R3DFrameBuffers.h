#ifndef FBO_H
#define FBO_H

#include <GL/glew.h>
#include "GLSLShader.h"
#include "Model.h"

namespace New3D {

class R3DFrameBuffers {

public:
	R3DFrameBuffers();
	~R3DFrameBuffers();

	void	Draw();					// draw and composite the transparent layers
	
	Result	CreateFBO(int width, int height);
	void	DestroyFBO();

	void	BindTexture(Layer layer);
	void	SetFBO(Layer layer);
	void	StoreDepth();

private:

	Result	CreateFBODepthCopy(int width, int height);
	GLuint	CreateTexture(int width, int height);
	void	AllocShaderComposite();

	GLuint m_frameBufferID;
	GLuint m_renderBufferID;
	GLuint m_texIDs[3];
	GLuint m_frameBufferIDCopy;
	GLuint m_renderBufferIDCopy;
	GLuint m_frameBufferIDTrans2;	// trans2 FBO: att2 color + copy depth, avoids RestoreDepth blit
	Layer m_lastLayer;
	int m_width;
	int m_height;

	// shaders
	GLSLShader m_shaderComposite;

	// vao
	GLuint m_vao;	// this really needed if we don't actually use vertex attribs?
};

}

#endif
