#include "R3DFrameBuffers.h"
#include <string>
#include "../GLSLVersion.h"

namespace New3D {

R3DFrameBuffers::R3DFrameBuffers()
{
	m_frameBufferID = 0;
	m_renderBufferID = 0;
	m_frameBufferIDCopy = 0;
	m_renderBufferIDCopy = 0;
	m_frameBufferIDTrans2 = 0;
	m_width = 0;
	m_height = 0;
	m_vao = 0;

	for (auto &i : m_texIDs) {
		i = 0;
	}

	m_lastLayer = Layer::none;

	AllocShaderComposite();

	glGenVertexArrays(1, &m_vao);
	glBindVertexArray(m_vao);
	// no states needed since we do it in the shader
	glBindVertexArray(0);
}

R3DFrameBuffers::~R3DFrameBuffers()
{
	DestroyFBO();
	m_shaderComposite.UnloadShaders();

	if (m_vao) {
		glDeleteVertexArrays(1, &m_vao);
		m_vao = 0;
	}
}

Result R3DFrameBuffers::CreateFBO(int width, int height)
{
	m_width = width;
	m_height = height;

	m_texIDs[0] = CreateTexture(width, height);		// colour buffer
	m_texIDs[1] = CreateTexture(width, height);		// trans layer1
	m_texIDs[2] = CreateTexture(width, height);		// trans layer2

	glGenFramebuffers(1, &m_frameBufferID);
	glBindFramebuffer(GL_FRAMEBUFFER, m_frameBufferID);

	// colour attachments
	glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, m_texIDs[0], 0);
	glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT1, GL_TEXTURE_2D, m_texIDs[1], 0);
	glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT2, GL_TEXTURE_2D, m_texIDs[2], 0);

	// depth/stencil attachment
	glGenRenderbuffers(1, &m_renderBufferID);
	glBindRenderbuffer(GL_RENDERBUFFER, m_renderBufferID);
	glRenderbufferStorage(GL_RENDERBUFFER, GL_DEPTH32F_STENCIL8, width, height);
	glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_DEPTH_STENCIL_ATTACHMENT, GL_RENDERBUFFER, m_renderBufferID);

	// check setup was successful
	auto fboStatus = glCheckFramebufferStatus(GL_FRAMEBUFFER);

	glBindFramebuffer(GL_FRAMEBUFFER, 0);	//created R3DFrameBuffers now disable it

	return ((CreateFBODepthCopy(width, height) == Result::OKAY) && (fboStatus == GL_FRAMEBUFFER_COMPLETE)) ? Result::OKAY : Result::FAIL;
}

Result R3DFrameBuffers::CreateFBODepthCopy(int width, int height)
{
	// Depth-only copy FBO — receives the opaque depth via StoreDepth blit
	glGenFramebuffers(1, &m_frameBufferIDCopy);
	glBindFramebuffer(GL_FRAMEBUFFER, m_frameBufferIDCopy);

	glGenRenderbuffers(1, &m_renderBufferIDCopy);
	glBindRenderbuffer(GL_RENDERBUFFER, m_renderBufferIDCopy);
	glRenderbufferStorage(GL_RENDERBUFFER, GL_DEPTH32F_STENCIL8, width, height);
	glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_DEPTH_STENCIL_ATTACHMENT, GL_RENDERBUFFER, m_renderBufferIDCopy);

	auto copyStatus = glCheckFramebufferStatus(GL_FRAMEBUFFER);

	// Trans2 FBO — shares the copy depth so SetFBO(trans2) needs no RestoreDepth blit
	glGenFramebuffers(1, &m_frameBufferIDTrans2);
	glBindFramebuffer(GL_FRAMEBUFFER, m_frameBufferIDTrans2);
	glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, m_texIDs[2], 0);
	glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_DEPTH_STENCIL_ATTACHMENT, GL_RENDERBUFFER, m_renderBufferIDCopy);

	auto trans2Status = glCheckFramebufferStatus(GL_FRAMEBUFFER);

	glBindFramebuffer(GL_FRAMEBUFFER, 0);

	return (copyStatus == GL_FRAMEBUFFER_COMPLETE && trans2Status == GL_FRAMEBUFFER_COMPLETE) ? Result::OKAY : Result::FAIL;
}

void R3DFrameBuffers::StoreDepth()
{
	glBindFramebuffer(GL_READ_FRAMEBUFFER, m_frameBufferID);
	glBindFramebuffer(GL_DRAW_FRAMEBUFFER, m_frameBufferIDCopy);
	glBlitFramebuffer(0, 0, m_width, m_height, 0, 0, m_width, m_height, GL_DEPTH_BUFFER_BIT | GL_STENCIL_BUFFER_BIT, GL_NEAREST);
}


void R3DFrameBuffers::DestroyFBO()
{
	if (m_frameBufferID) {
		glBindFramebuffer(GL_FRAMEBUFFER, 0);
		glDeleteRenderbuffers(1, &m_renderBufferID);
		glDeleteFramebuffers(1, &m_frameBufferID);
	}

	if (m_frameBufferIDCopy) {
		glDeleteRenderbuffers(1, &m_renderBufferIDCopy);
		glDeleteFramebuffers(1, &m_frameBufferIDCopy);
	}

	if (m_frameBufferIDTrans2) {
		glDeleteFramebuffers(1, &m_frameBufferIDTrans2);
	}

	for (auto &i : m_texIDs) {
		if (i) {
			glDeleteTextures(1, &i);
			i = 0;
		}
	}

	m_frameBufferID = 0;
	m_renderBufferID = 0;
	m_frameBufferIDCopy = 0;
	m_renderBufferIDCopy = 0;
	m_frameBufferIDTrans2 = 0;
	m_width = 0;
	m_height = 0;
}

GLuint R3DFrameBuffers::CreateTexture(int width, int height)
{
	GLuint texId;
	glGenTextures(1, &texId);
	glBindTexture(GL_TEXTURE_2D, texId);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
	glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, width, height, 0, GL_RGBA, GL_UNSIGNED_BYTE, 0);
	return texId;
}

void R3DFrameBuffers::BindTexture(Layer layer)
{
	glBindTexture(GL_TEXTURE_2D, m_texIDs[(int)layer]);
}

void R3DFrameBuffers::SetFBO(Layer layer)
{
	if (m_lastLayer == layer) {
		return;
	}

	switch (layer)
	{
	case Layer::colour:
	{
		glBindFramebuffer(GL_FRAMEBUFFER, m_frameBufferID);
		GLenum buffers[] = { GL_COLOR_ATTACHMENT0, GL_COLOR_ATTACHMENT1, GL_COLOR_ATTACHMENT2 };
		glDrawBuffers((GLsizei)std::size(buffers), buffers);
		break;
	}
	case Layer::trans1:
	{
		glBindFramebuffer(GL_FRAMEBUFFER, m_frameBufferID);
		GLenum buffers[] = { GL_NONE, GL_COLOR_ATTACHMENT1, GL_NONE };
		glDrawBuffers((GLsizei)std::size(buffers), buffers);
		break;
	}
	case Layer::trans2:
	{
		// m_frameBufferIDTrans2 has att2 color at slot 0 + copy depth (= opaque depth from StoreDepth)
		glBindFramebuffer(GL_FRAMEBUFFER, m_frameBufferIDTrans2);
		GLenum buffers[] = { GL_COLOR_ATTACHMENT0 };
		glDrawBuffers((GLsizei)std::size(buffers), buffers);
		break;
	}
	case Layer::none:
	{
		glBindFramebuffer(GL_FRAMEBUFFER, 0);
		glDrawBuffer(GL_BACK);
		break;
	}
	}

	m_lastLayer = layer;
}

void R3DFrameBuffers::AllocShaderComposite()
{
	static const char *vertexShader = R"glsl(

	void main(void)
	{
		const vec4 vertices[] = vec4[](vec4(-1.0, -1.0, 0.0, 1.0),
										vec4(-1.0,  1.0, 0.0, 1.0),
										vec4( 1.0, -1.0, 0.0, 1.0),
										vec4( 1.0,  1.0, 0.0, 1.0));

		gl_Position = vertices[gl_VertexID % 4];
	}

	)glsl";

	static const char *fragmentShader = R"glsl(

	uniform sampler2D tex0;		// opaque layer
	uniform sampler2D tex1;		// trans layer 1
	uniform sampler2D tex2;		// trans layer 2

	out vec4 fragColor;

	void main()
	{
		ivec2 tc = ivec2(gl_FragCoord.xy);
		vec4 base       = texelFetch(tex0, tc, 0);
		vec4 colTrans1  = texelFetch(tex1, tc, 0);
		vec4 colTrans2  = texelFetch(tex2, tc, 0);

		vec4 trans;
		if (colTrans1.a * colTrans2.a > 0.0) {
			vec3 mixCol = mix(colTrans1.rgb, colTrans2.rgb, (colTrans2.a + (1.0 - colTrans1.a)) / 2.0);
			trans = vec4(mixCol, 1.0);
		}
		else if (colTrans1.a > 0.0) {
			trans = colTrans1;
		}
		else {
			trans = colTrans2;
		}

		// Premultiplied-alpha output equivalent to the old two-pass blend:
		//   pass1: GL_SRC_ALPHA on base (att0.a is 1.0 for geometry, 0 for sky/background)
		//   pass2: GL_SRC_ALPHA on trans composited on top
		// Combined with GL_ONE / GL_ONE_MINUS_SRC_ALPHA at the draw call.
		vec3  outRGB   = trans.a * trans.rgb + (1.0 - trans.a) * base.a * base.rgb;
		float outAlpha = trans.a + base.a - trans.a * base.a;
		fragColor = vec4(outRGB, outAlpha);
	}

	)glsl";

	std::string vs = Graphics::GLSLVersion::Get3D() + vertexShader;
	std::string fs = Graphics::GLSLVersion::Get3D() + fragmentShader;
	m_shaderComposite.LoadShaders(vs.c_str(), fs.c_str());
	m_shaderComposite.uniformLoc[0] = m_shaderComposite.GetUniformLocation("tex0");
	m_shaderComposite.uniformLoc[1] = m_shaderComposite.GetUniformLocation("tex1");
	m_shaderComposite.uniformLoc[2] = m_shaderComposite.GetUniformLocation("tex2");
}

void R3DFrameBuffers::Draw()
{
	glViewport		(0, 0, m_width, m_height);
	glDisable		(GL_DEPTH_TEST);
	glDisable		(GL_CULL_FACE);
	glBlendFunc		(GL_ONE, GL_ONE_MINUS_SRC_ALPHA);	// premultiplied-alpha blend
	glEnable		(GL_BLEND);

	for (int i = 0; i < (int)std::size(m_texIDs); i++) {
		glActiveTexture(GL_TEXTURE0 + i);
		glBindTexture(GL_TEXTURE_2D, m_texIDs[i]);
	}

	glActiveTexture(GL_TEXTURE0);
	glBindVertexArray(m_vao);

	m_shaderComposite.EnableShader();
	glUniform1i(m_shaderComposite.uniformLoc[0], 0);
	glUniform1i(m_shaderComposite.uniformLoc[1], 1);
	glUniform1i(m_shaderComposite.uniformLoc[2], 2);
	glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
	m_shaderComposite.DisableShader();

	glDisable(GL_BLEND);
	glBindVertexArray(0);
}

}
