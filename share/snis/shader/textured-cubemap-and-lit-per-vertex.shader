

#if !defined(AMBIENT)
#define AMBIENT 0.1
#endif

#if defined(INCLUDE_VS)
	out vec3 v_TexCoord;
	out vec3 v_LightColor;
	uniform mat4 u_MVPMatrix;  // A constant representing the combined model/view/projection matrix.
	uniform mat4 u_MVMatrix;   // A constant representing the combined model/view matrix.
	uniform mat3 u_NormalMatrix;
	uniform vec3 u_LightPos;   // The position of the light in eye space.

	in vec4 a_Position; // Per-vertex position information we will pass in.
	in vec3 a_Normal;   // Per-vertex normal information we will pass in.

	void main()
	{
		// Transform the vertex into eye space.
		vec3 position = vec3(u_MVMatrix * a_Position);

		// Transform the normal's orientation into eye space.
		vec3 normal = normalize(u_NormalMatrix * a_Normal);

		// Get a lighting direction vector from the light to the vertex.
		vec3 light_dir = normalize(u_LightPos - position);

		// Calculate the dot product of the light vector and vertex normal. If the normal and light vector are
		// pointing in the same direction then it will get max illumination.
		float diffuse = max(AMBIENT, dot(normal, light_dir));

		// Multiply the color by the illumination level. It will be interpolated across the triangle.
		v_LightColor = vec3(diffuse);
		v_TexCoord = a_Position.xyz;

		gl_Position = u_MVPMatrix * a_Position;
	}
#endif

#if defined(INCLUDE_FS)
	in vec3 v_TexCoord;
	in vec3 v_LightColor;
	uniform samplerCube u_AlbedoTex;
	uniform samplerCube u_NormalMapTex;
	uniform vec4 u_TintColor;

	void main()
	{
		gl_FragColor = vec4(v_LightColor) * textureCube(u_AlbedoTex, v_TexCoord);

		/* tint with alpha premultiply */
		gl_FragColor.rgb *= u_TintColor.rgb;
		gl_FragColor *= u_TintColor.a;
	}
#endif

