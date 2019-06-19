#ifndef RENDERING_DEVICE_VULKAN_H
#define RENDERING_DEVICE_VULKAN_H

#include "core/oa_hash_map.h"
#include "core/os/thread_safe.h"
#include "core/rid_owner.h"
#include "servers/visual/rendering_device.h"
#include "thirdparty/glslang/glslang/Public/ShaderLang.h"
#include "vk_mem_alloc.h"
#include <vulkan/vulkan.h>
//todo:
//compute
//push constants
//views of texture slices

class VulkanContext;

class RenderingDeviceVulkan : public RenderingDevice {

	_THREAD_SAFE_CLASS_

	// Miscellaneous tables that map
	// our enums to enums used
	// by vulkan.

	VkPhysicalDeviceLimits limits;
	static const VkFormat vulkan_formats[DATA_FORMAT_MAX];
	static const char *named_formats[DATA_FORMAT_MAX];
	static const VkCompareOp compare_operators[COMPARE_OP_MAX];
	static const VkStencilOp stencil_operations[STENCIL_OP_MAX];
	static const VkSampleCountFlagBits rasterization_sample_count[TEXTURE_SAMPLES_MAX];
	static const VkLogicOp logic_operations[RenderingDevice::LOGIC_OP_MAX];
	static const VkBlendFactor blend_factors[RenderingDevice::BLEND_FACTOR_MAX];
	static const VkBlendOp blend_operations[RenderingDevice::BLEND_OP_MAX];
	static const VkSamplerAddressMode address_modes[SAMPLER_REPEAT_MODE_MAX];
	static const VkBorderColor sampler_border_colors[SAMPLER_BORDER_COLOR_MAX];

	// Functions used for format
	// validation, and ensures the
	// user passes valid data.

	static int get_format_vertex_size(DataFormat p_format);
	static uint32_t get_image_format_pixel_size(DataFormat p_format);
	static void get_compressed_image_format_block_dimensions(DataFormat p_format, uint32_t &r_w, uint32_t &r_h);
	uint32_t get_compressed_image_format_block_byte_size(DataFormat p_format);
	static uint32_t get_compressed_image_format_pixel_rshift(DataFormat p_format);
	static uint32_t get_image_format_required_size(DataFormat p_format, uint32_t p_width, uint32_t p_height, uint32_t p_depth, uint32_t p_mipmaps, uint32_t *r_blockw = NULL, uint32_t *r_blockh = NULL);
	static uint32_t get_image_required_mipmaps(uint32_t p_width, uint32_t p_height, uint32_t p_depth);

	/***************************/
	/**** ID INFRASTRUCTURE ****/
	/***************************/

	enum IDType {
		ID_TYPE_FRAMEBUFFER_FORMAT,
		ID_TYPE_VERTEX_FORMAT,
		ID_TYPE_DRAW_LIST,
		ID_TYPE_SPLIT_DRAW_LIST,
		ID_TYPE_MAX,
		ID_BASE_SHIFT = 58 //5 bits for ID types
	};

	VkDevice device;

	Map<RID, Set<RID> > dependency_map; //IDs to IDs that depend on it
	Map<RID, Set<RID> > reverse_dependency_map; //same as above, but in reverse

	void _add_dependency(RID p_id, RID p_depends_on);
	void _free_dependencies(RID p_id);

	/*****************/
	/**** TEXTURE ****/
	/*****************/

	// In Vulkan, the concept of textures does not exist,
	// intead there is the image (the memory prety much,
	// the view (how the memory is interpreted) and the
	// sampler (how it's sampled from the shader).
	//
	// Texture here includes the first two stages, but
	// It's possible to create textures sharing the image
	// but with different views. The main use case for this
	// is textures that can be read as both SRGB/Linear,
	// or slices of a texture (a mipmap, a layer, a 3D slice)
	// for a framebuffer to render into it.

	struct Texture {

		VkImage image;
		VmaAllocation allocation;
		VmaAllocationInfo allocation_info;
		VkImageView view;

		TextureType type;
		DataFormat format;
		TextureSamples samples;
		uint32_t width;
		uint32_t height;
		uint32_t depth;
		uint32_t layers;
		uint32_t mipmaps;
		uint32_t usage_flags;

		Vector<DataFormat> allowed_shared_formats;

		VkImageLayout bound_layout; //layout used when bound to framebuffer being drawn
		VkImageLayout unbound_layout; //layout used otherwise
		uint32_t aspect_mask;
		bool bound; //bound to framebffer
		RID owner;
	};

	RID_Owner<Texture> texture_owner;
	uint32_t texture_upload_region_size_px;

	/*****************/
	/**** SAMPLER ****/
	/*****************/

	RID_Owner<VkSampler> sampler_owner;

	/***************************/
	/**** BUFFER MANAGEMENT ****/
	/***************************/

	// These are temporary buffers on CPU memory that hold
	// the information until the CPU fetches it and places it
	// either on GPU buffers, or images (textures). It ensures
	// updates are properly synchronized with whathever the
	// GPU is doing.
	//
	// The logic here is as follows, only 3 of these
	// blocks are created at the beginning (one per frame)
	// they can each belong to a frame (assigned to current when
	// used) and they can only be reused after the same frame is
	// recycled.
	//
	// When CPU requires to allocate more than what is available,
	// more of these buffers are created. If a limit is reached,
	// then a fence will ensure will wait for blocks allocated
	// in previous frames are processed. If that fails, then
	// another fence will ensure everything pending for the current
	// frame is processed (effectively stalling).
	//
	// See the comments in the code to understand better how it works.

	struct StagingBufferBlock {
		VkBuffer buffer;
		VmaAllocation allocation;
		uint64_t frame_used;
		uint32_t fill_amount;
	};

	Vector<StagingBufferBlock> staging_buffer_blocks;
	int staging_buffer_current;
	uint32_t staging_buffer_block_size;
	uint64_t staging_buffer_max_size;
	bool staging_buffer_used;

	Error _staging_buffer_allocate(uint32_t p_amount, uint32_t p_required_align, uint32_t &r_alloc_offset, uint32_t &r_alloc_size, bool p_can_segment = true, bool p_on_draw_command_buffer = false);
	Error _insert_staging_block();

	struct Buffer {

		uint32_t size;
		VkBuffer buffer;
		VmaAllocation allocation;
		VkDescriptorBufferInfo buffer_info; //used for binding
		Buffer() {
			size = 0;
			buffer = NULL;
			allocation = NULL;
		}
	};

	Error _buffer_allocate(Buffer *p_buffer, uint32_t p_size, uint32_t p_usage, VmaMemoryUsage p_mapping);
	Error _buffer_free(Buffer *p_buffer);
	Error _buffer_update(Buffer *p_buffer, size_t p_offset, const uint8_t *p_data, size_t p_data_size, bool p_use_draw_command_buffer = false, uint32_t p_required_align = 32);

	/*********************/
	/**** FRAMEBUFFER ****/
	/*********************/

	// In Vulkan, framebuffers work similar to how they
	// do in OpenGL, with the exception that
	// the "format" (vkRenderPass) is not dynamic
	// and must be more or less the same as the one
	// used for the render pipelines.

	struct FramebufferFormatKey {
		Vector<AttachmentFormat> attachments;
		bool operator<(const FramebufferFormatKey &p_key) const {

			int as = attachments.size();
			int bs = p_key.attachments.size();
			if (as != bs) {
				return as < bs;
			}

			const AttachmentFormat *af_a = attachments.ptr();
			const AttachmentFormat *af_b = p_key.attachments.ptr();
			for (int i = 0; i < as; i++) {
				const AttachmentFormat &a = af_a[i];
				const AttachmentFormat &b = af_b[i];
				if (a.format != b.format) {
					return a.format < b.format;
				}
				if (a.samples != b.samples) {
					return a.samples < b.samples;
				}
				if (a.usage_flags != b.usage_flags) {
					return a.usage_flags < b.usage_flags;
				}
			}

			return false; //equal
		}
	};

	VkRenderPass _render_pass_create(const Vector<AttachmentFormat> &p_format, InitialAction p_initial_action, FinalAction p_final_action, int *r_color_attachment_count = NULL);

	// This is a cache and it's never freed, it ensures
	// IDs for a given format are always unique.
	Map<FramebufferFormatKey, FramebufferFormatID> framebuffer_format_cache;
	struct FramebufferFormat {
		const Map<FramebufferFormatKey, FramebufferFormatID>::Element *E;
		VkRenderPass render_pass; //here for constructing shaders, never used, see section (7.2. Render Pass Compatibility from Vulkan spec)
		int color_attachments; //used for pipeline validation
	};

	Map<FramebufferFormatID, FramebufferFormat> framebuffer_formats;

	struct Framebuffer {
		FramebufferFormatID format_id;
		struct VersionKey {
			InitialAction initial_action;
			FinalAction final_action;
			bool operator<(const VersionKey &p_key) const {
				if (initial_action == p_key.initial_action) {
					return final_action < p_key.final_action;
				} else {
					return initial_action < p_key.initial_action;
				}
			}
		};

		Vector<RID> texture_ids;

		struct Version {
			VkFramebuffer framebuffer;
			VkRenderPass render_pass; //this one is owned
		};

		Map<VersionKey, Version> framebuffers;
		Size2 size;
	};

	RID_Owner<Framebuffer> framebuffer_owner;

	/***********************/
	/**** VERTEX BUFFER ****/
	/***********************/

	// Vertex buffers in Vulkan are similar to how
	// they work in OpenGL, except that instead of
	// an attribtue index, there is a buffer binding
	// index (for binding the buffers in real-time)
	// and a location index (what is used in the shader).
	//
	// This mapping is done here internally, and it's not
	// exposed.

	RID_Owner<Buffer> vertex_buffer_owner;

	struct VertexDescriptionKey {
		Vector<VertexDescription> vertex_formats;
		int buffer_count;
		bool operator<(const VertexDescriptionKey &p_key) const {
			if (buffer_count != p_key.buffer_count) {
				return buffer_count < p_key.buffer_count;
			}
			if (vertex_formats.size() != p_key.vertex_formats.size()) {
				return vertex_formats.size() < p_key.vertex_formats.size();
			} else {
				int vdc = vertex_formats.size();
				const VertexDescription *a_ptr = vertex_formats.ptr();
				const VertexDescription *b_ptr = p_key.vertex_formats.ptr();
				for (int i = 0; i < vdc; i++) {
					const VertexDescription &a = a_ptr[i];
					const VertexDescription &b = b_ptr[i];

					if (a.location != b.location) {
						return a.location < b.location;
					}
					if (a.offset != b.offset) {
						return a.offset < b.offset;
					}
					if (a.format != b.format) {
						return a.format < b.format;
					}
					if (a.stride != b.stride) {
						return a.stride < b.stride;
					}
					return a.frequency < b.frequency;
				}
				return false; //they are equal
			}
		}
	};

	// This is a cache and it's never freed, it ensures that
	// ID used for a specific format always remain the same.
	Map<VertexDescriptionKey, VertexFormatID> vertex_format_cache;
	struct VertexDescriptionCache {
		const Map<VertexDescriptionKey, VertexFormatID>::Element *E;
		VkVertexInputBindingDescription *bindings;
		VkVertexInputAttributeDescription *attributes;
		VkPipelineVertexInputStateCreateInfo create_info;
	};

	Map<VertexFormatID, VertexDescriptionCache> vertex_formats;

	struct VertexArray {
		RID buffer;
		VertexFormatID description;
		int vertex_count;
		uint32_t max_instances_allowed;

		Vector<VkBuffer> buffers; //not owned, just referenced
		Vector<VkDeviceSize> offsets;
	};

	RID_Owner<VertexArray> vertex_array_owner;

	struct IndexBuffer : public Buffer {
		uint32_t max_index; //used for validation
		uint32_t index_count;
		VkIndexType index_type;
		bool supports_restart_indices;
	};

	RID_Owner<IndexBuffer> index_buffer_owner;

	struct IndexArray {
		uint32_t max_index; //remember the maximum index here too, for validation
		VkBuffer buffer; //not owned, inherited from index buffer
		uint32_t offset;
		uint32_t indices;
		VkIndexType index_type;
		bool supports_restart_indices;
	};

	RID_Owner<IndexArray> index_array_owner;

	/****************/
	/**** SHADER ****/
	/****************/

	// Shaders in Vulkan are just pretty much
	// precompiled blocks of SPIR-V bytecode. They
	// are most likely not really compiled to host
	// assembly until a pipeline is created.
	//
	// When supplying the shaders, this implementation
	// will use the reflection abilities of glslang to
	// understand and cache everything required to
	// create and use the descriptor sets (Vulkan's
	// biggest pain).
	//
	// Additionally, hashes are created for every set
	// to do quick validation and ensuring the user
	// does not submit something invalid.

	struct Shader {

		struct UniformInfo {
			UniformType type;
			int binding;
			uint32_t stages;
			int length; //size of arrays (in total elements), or ubos (in bytes * total elements)
			bool operator<(const UniformInfo &p_info) const {
				if (type != p_info.type) {
					return type < p_info.type;
				}
				if (binding != p_info.binding) {
					return binding < p_info.binding;
				}
				if (stages != p_info.stages) {
					return stages < p_info.stages;
				}
				return length < p_info.length;
			}
		};

		struct Set {

			Vector<UniformInfo> uniform_info;
			VkDescriptorSetLayout descriptor_set_layout;
		};

		Vector<int> vertex_input_locations; //inputs used, this is mostly for validation
		int fragment_outputs;

		struct PushConstant {
			uint32_t push_constant_size;
			uint32_t push_constants_vk_stage;
		};

		PushConstant push_constant;

		int max_output;
		Vector<Set> sets;
		Vector<uint32_t> set_hashes;
		Vector<VkPipelineShaderStageCreateInfo> pipeline_stages;
		VkPipelineLayout pipeline_layout;
	};

	bool _uniform_add_binding(Vector<Vector<VkDescriptorSetLayoutBinding> > &bindings, Vector<Vector<Shader::UniformInfo> > &uniform_infos, const glslang::TObjectReflection &reflection, RenderingDevice::ShaderStage p_stage, Shader::PushConstant &push_constant, String *r_error);

	RID_Owner<Shader> shader_owner;

	/******************/
	/**** UNIFORMS ****/
	/******************/

	// Descriptor sets require allocation from a pool.
	// The documentation on how to use pools properly
	// is scarce, and the documentation is strange.
	//
	// Basically, you can mix and match pools as you
	// like, but you'll run into fragmentation issues.
	// Because of this, the recommended approach is to
	// create a a pool for every descriptor set type,
	// as this prevents fragmentation.
	//
	// This is implemented here as a having a list of
	// pools (each can contain up to 64 sets) for each
	// set layout. The amount of sets for each type
	// is used as the key.

	enum {
		MAX_DESCRIPTOR_POOL_ELEMENT = 65535
	};

	struct DescriptorPoolKey {
		union {
			struct {
				uint16_t uniform_type[UNIFORM_TYPE_MAX]; //using 16 bits because, for sending arrays, each element is a pool set.
			};
			struct {
				uint64_t key1;
				uint64_t key2;
				uint64_t key3;
			};
		};
		bool operator<(const DescriptorPoolKey &p_key) const {
			if (key1 != p_key.key1) {
				return key1 < p_key.key1;
			}
			if (key2 != p_key.key2) {
				return key2 < p_key.key2;
			}

			return key3 < p_key.key3;
		}
		DescriptorPoolKey() {
			key1 = 0;
			key2 = 0;
			key3 = 0;
		}
	};

	struct DescriptorPool {
		VkDescriptorPool pool;
		uint32_t usage;
	};

	Map<DescriptorPoolKey, Set<DescriptorPool *> > descriptor_pools;
	uint32_t max_descriptors_per_pool;

	DescriptorPool *_descriptor_pool_allocate(const DescriptorPoolKey &p_key);
	void _descriptor_pool_free(const DescriptorPoolKey &p_key, DescriptorPool *p_pool);

	RID_Owner<Buffer> uniform_buffer_owner;
	RID_Owner<Buffer> storage_buffer_owner;

	//texture buffer needs a view
	struct TextureBuffer {
		Buffer buffer;
		VkBufferView view;
	};

	RID_Owner<TextureBuffer> texture_buffer_owner;

	// This structure contains the descriptor set. They _need_ to be allocated
	// for a shader (and will be erased when this shader is erased), but should
	// work for other shaders as long as the hash matches. This covers using
	// them in shader variants.
	//
	// Keep also in mind that you can share buffers between descriptor sets, so
	// the above restriction is not too serious.

	struct UniformSet {
		uint32_t hash;
		RID shader_id;
		DescriptorPool *pool;
		DescriptorPoolKey pool_key;
		VkDescriptorSet descriptor_set;
		VkPipelineLayout pipeline_layout; //not owned, inherited from shader
		Vector<RID> attachable_textures; //used for validation
	};

	RID_Owner<UniformSet> uniform_set_owner;

	/*******************/
	/**** PIPELINES ****/
	/*******************/

	// Render pipeline contains ALL the
	// information required for drawing.
	// This includes all the rasterizer state
	// as well as shader used, framebuffer format,
	// etc.
	// While the pipeline is just a single object
	// (VkPipeline) a lot of values are also saved
	// here to do validation (vulkan does none by
	// default) and warn the user if something
	// was not supplied as intended.

	struct RenderPipeline {
		//Cached values for validation
		FramebufferFormatID framebuffer_format;
		uint32_t dynamic_state;
		VertexFormatID vertex_format;
		bool uses_restart_indices;
		uint32_t primitive_minimum;
		uint32_t primitive_divisor;
		Vector<uint32_t> set_hashes;
		uint32_t push_constant_size;
		uint32_t push_constant_stages;
		//Actual pipeline
		VkPipelineLayout pipeline_layout; // not owned, needed for push constants
		VkPipeline pipeline;
	};

	RID_Owner<RenderPipeline> pipeline_owner;

	/*******************/
	/**** DRAW LIST ****/
	/*******************/

	// Draw list contains both the command buffer
	// used for drawing as well as a LOT of
	// information used for validation. This
	// validation is cheap so most of it can
	// also run in release builds.

	// When using split command lists, this is
	// implemented internally using secondary command
	// buffers. As they can be created in threads,
	// each needs it's own command pool.

	struct SplitDrawListAllocator {
		VkCommandPool command_pool;
		Vector<VkCommandBuffer> command_buffers; //one for each frame
	};

	Vector<SplitDrawListAllocator> split_draw_list_allocators;

	struct DrawList {

		VkCommandBuffer command_buffer; //if persistent, this is owned, otherwise it's shared with the ringbuffer

		struct Validation {
			bool active; //means command buffer was not closes, so you can keep adding things
			FramebufferFormatID framebuffer_format;
			//actual render pass values
			uint32_t dynamic_state;
			VertexFormatID vertex_format; //INVALID_ID if not set
			uint32_t vertex_array_size; //0 if not set
			uint32_t vertex_max_instances_allowed;
			bool index_buffer_uses_restart_indices;
			uint32_t index_array_size; //0 if index buffer not set
			uint32_t index_array_max_index;
			uint32_t index_array_offset;
			Vector<uint32_t> set_hashes;
			//last pipeline set values
			bool pipeline_active;
			uint32_t pipeline_dynamic_state;
			VertexFormatID pipeline_vertex_format;
			bool pipeline_uses_restart_indices;
			uint32_t pipeline_primitive_divisor;
			uint32_t pipeline_primitive_minimum;
			Vector<uint32_t> pipeline_set_hashes;
			VkPipelineLayout pipeline_push_constant_layout;
			uint32_t pipeline_push_constant_size;
			uint32_t pipeline_push_constant_stages;
			bool pipeline_push_constant_suppplied;

			Validation() {
				active = true;
				dynamic_state = 0;
				vertex_format = INVALID_ID;
				vertex_array_size = 0;
				vertex_max_instances_allowed = 0xFFFFFFFF;
				framebuffer_format = INVALID_ID;
				index_array_size = 0; //not sent
				index_array_max_index = 0; //not set
				index_buffer_uses_restart_indices = false;

				//pipeline state initalize
				pipeline_active = false;
				pipeline_dynamic_state = 0;
				pipeline_vertex_format = INVALID_ID;
				pipeline_uses_restart_indices = false;
				pipeline_push_constant_size = 0;
				pipeline_push_constant_stages = 0;
				pipeline_push_constant_suppplied = false;
			}
		} validation;
	};

	DrawList *draw_list; //one for regular draw lists, multiple for split.
	uint32_t draw_list_count;
	bool draw_list_split;
	Vector<RID> draw_list_bound_textures;
	bool draw_list_unbind_textures;

	Error _draw_list_setup_framebuffer(Framebuffer *p_framebuffer, InitialAction p_initial_action, FinalAction p_final_action, VkFramebuffer *r_framebuffer, VkRenderPass *r_render_pass);
	Error _draw_list_render_pass_begin(Framebuffer *framebuffer, InitialAction p_initial_action, FinalAction p_final_action, const Vector<Color> &p_clear_colors, Point2i viewport_offset, Point2i viewport_size, VkFramebuffer vkframebuffer, VkRenderPass render_pass, VkCommandBuffer command_buffer, VkSubpassContents subpass_contents);
	_FORCE_INLINE_ DrawList *_get_draw_list_ptr(DrawListID p_id);

	/**************************/
	/**** FRAME MANAGEMENT ****/
	/**************************/

	// This is the frame structure. There are normally
	// 3 of these (used for triple buffering), or 2
	// (double buffering). They are cycled constantly.
	//
	// It contains two command buffers, one that is
	// used internally for setting up (creating stuff)
	// and another used mostly for drawing.
	//
	// They also contains a list of things that need
	// to be disposed of when deleted, which can't
	// happen immediately due to the asynchronous
	// nature of the GPU. They will get deleted
	// when the frame is cycled.

	struct Frame {
		//list in usage order, from last to free to first to free
		List<Buffer> buffers_to_dispose_of;
		List<Texture> textures_to_dispose_of;
		List<Framebuffer> framebuffers_to_dispose_of;
		List<VkSampler> samplers_to_dispose_of;
		List<Shader> shaders_to_dispose_of;
		List<VkBufferView> buffer_views_to_dispose_of;
		List<UniformSet> uniform_sets_to_dispose_of;
		List<RenderPipeline> pipelines_to_dispose_of;

		VkCommandPool command_pool;
		VkCommandBuffer setup_command_buffer; //used at the begining of every frame for set-up
		VkCommandBuffer draw_command_buffer; //used at the begining of every frame for set-up
	};

	Frame *frames; //frames available, they are cycled (usually 3)
	int frame; //current frame
	int frame_count; //total amount of frames
	uint64_t frames_drawn;

	void _free_pending_resources();

	VmaAllocator allocator;

	VulkanContext *context;

	void _free_internal(RID p_id);

public:
	virtual RID texture_create(const TextureFormat &p_format, const TextureView &p_view, const Vector<PoolVector<uint8_t> > &p_data = Vector<PoolVector<uint8_t> >());
	virtual RID texture_create_shared(const TextureView &p_view, RID p_with_texture);
	virtual Error texture_update(RID p_texture, uint32_t p_layer, const PoolVector<uint8_t> &p_data, bool p_sync_with_draw = false);

	virtual bool texture_is_format_supported_for_usage(DataFormat p_format, uint32_t p_usage) const;

	/*********************/
	/**** FRAMEBUFFER ****/
	/*********************/

	FramebufferFormatID framebuffer_format_create(const Vector<AttachmentFormat> &p_format);

	virtual RID framebuffer_create(const Vector<RID> &p_texture_attachments, FramebufferFormatID p_format_check = INVALID_ID);

	virtual FramebufferFormatID framebuffer_get_format(RID p_framebuffer);

	/*****************/
	/**** SAMPLER ****/
	/*****************/

	virtual RID sampler_create(const SamplerState &p_state);

	/**********************/
	/**** VERTEX ARRAY ****/
	/**********************/

	virtual RID vertex_buffer_create(uint32_t p_size_bytes, const PoolVector<uint8_t> &p_data = PoolVector<uint8_t>());

	// Internally reference counted, this ID is warranted to be unique for the same description, but needs to be freed as many times as it was allocated
	virtual VertexFormatID vertex_format_create(const Vector<VertexDescription> &p_vertex_formats);
	virtual RID vertex_array_create(uint32_t p_vertex_count, VertexFormatID p_vertex_format, const Vector<RID> &p_src_buffers);

	virtual RID index_buffer_create(uint32_t p_size_indices, IndexBufferFormat p_format, const PoolVector<uint8_t> &p_data = PoolVector<uint8_t>(), bool p_use_restart_indices = false);

	virtual RID index_array_create(RID p_index_buffer, uint32_t p_index_offset, uint32_t p_index_count);

	/****************/
	/**** SHADER ****/
	/****************/

	virtual RID shader_create_from_source(const Vector<ShaderStageSource> &p_stages, String *r_error = NULL, ShaderStage *r_error_stage = NULL, bool p_allow_cache = true);
	virtual Vector<int> shader_get_vertex_input_locations_used(RID p_shader);

	/*****************/
	/**** UNIFORM ****/
	/*****************/

	virtual RID uniform_buffer_create(uint32_t p_size_bytes, const PoolVector<uint8_t> &p_data = PoolVector<uint8_t>());
	virtual RID storage_buffer_create(uint32_t p_size_bytes, const PoolVector<uint8_t> &p_data = PoolVector<uint8_t>());
	virtual RID texture_buffer_create(uint32_t p_size_elements, DataFormat p_format, const PoolVector<uint8_t> &p_data = PoolVector<uint8_t>());

	virtual RID uniform_set_create(const Vector<Uniform> &p_uniforms, RID p_shader, uint32_t p_shader_set);
	virtual bool uniform_set_is_valid(RID p_uniform_set);

	virtual Error buffer_update(RID p_buffer, uint32_t p_offset, uint32_t p_size, void *p_data, bool p_sync_with_draw = false); //works for any buffer

	/*************************/
	/**** RENDER PIPELINE ****/
	/*************************/

	virtual RID render_pipeline_create(RID p_shader, FramebufferFormatID p_framebuffer_format, VertexFormatID p_vertex_format, RenderPrimitive p_render_primitive, const PipelineRasterizationState &p_rasterization_state, const PipelineMultisampleState &p_multisample_state, const PipelineDepthStencilState &p_depth_stencil_state, const PipelineColorBlendState &p_blend_state, int p_dynamic_state_flags = 0);

	/****************/
	/**** SCREEN ****/
	/****************/

	virtual int screen_get_width(int p_screen = 0) const;
	virtual int screen_get_height(int p_screen = 0) const;
	virtual FramebufferFormatID screen_get_framebuffer_format() const;

	/********************/
	/**** DRAW LISTS ****/
	/********************/

	virtual DrawListID draw_list_begin_for_screen(int p_screen = 0, const Color &p_clear_color = Color());
	virtual DrawListID draw_list_begin(RID p_framebuffer, InitialAction p_initial_action, FinalAction p_final_action, const Vector<Color> &p_clear_colors = Vector<Color>(), const Rect2 &p_region = Rect2());
	virtual Error draw_list_begin_split(RID p_framebuffer, uint32_t p_splits, DrawListID *r_split_ids, InitialAction p_initial_action, FinalAction p_final_action, const Vector<Color> &p_clear_colors = Vector<Color>(), const Rect2 &p_region = Rect2());

	virtual void draw_list_bind_render_pipeline(DrawListID p_list, RID p_render_pipeline);
	virtual void draw_list_bind_uniform_set(DrawListID p_list, RID p_uniform_set, uint32_t p_index);
	virtual void draw_list_bind_vertex_array(DrawListID p_list, RID p_vertex_array);
	virtual void draw_list_bind_index_array(DrawListID p_list, RID p_index_array);
	virtual void draw_list_set_push_constant(DrawListID p_list, void *p_data, uint32_t p_data_size);

	virtual void draw_list_draw(DrawListID p_list, bool p_use_indices, uint32_t p_instances = 1);

	virtual void draw_list_enable_scissor(DrawListID p_list, const Rect2 &p_rect);
	virtual void draw_list_disable_scissor(DrawListID p_list);

	virtual void draw_list_end();

	/**************/
	/**** FREE ****/
	/**************/

	virtual void free(RID p_id);

	void initialize(VulkanContext *p_context);
	void finalize();

	virtual void finalize_frame();
	virtual void advance_frame();

	RenderingDeviceVulkan();
};

#endif // RENDERING_DEVICE_VULKAN_H
