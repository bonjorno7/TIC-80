#include "system.h"
#include "net.h"
#include "tools.h"

#include <stdlib.h>
#include <stdio.h>
#include <time.h>

#include <SDL_gpu.h>

#if defined(__EMSCRIPTEN__)
#include <emscripten.h>
#endif

#define STUDIO_UI_SCALE 3
#define STUDIO_PIXEL_FORMAT GPU_FORMAT_RGBA
#define TEXTURE_SIZE (TIC80_FULLWIDTH)
#define OFFSET_LEFT ((TIC80_FULLWIDTH-TIC80_WIDTH)/2)
#define OFFSET_TOP ((TIC80_FULLHEIGHT-TIC80_HEIGHT)/2)

static struct
{
	Studio* studio;

	SDL_Window* window;

	struct
	{
		GPU_Target* screen;
		GPU_Image* texture;
		u32 shader;
		GPU_ShaderBlock block;
	} gpu;

	struct
	{
		SDL_Joystick* ports[TIC_GAMEPADS];
		GPU_Image* texture;

		tic80_gamepads touch;
		tic80_gamepads joystick;

		bool show;
		s32 counter;
		s32 alpha;

		struct
		{
			s32 size;
			SDL_Point axis;
			SDL_Point a;
			SDL_Point b;
			SDL_Point x;
			SDL_Point y;
		} part;
	} gamepad;

	struct
	{
		GPU_Image* texture;
		const u8* src;
	} mouse;

	Net* net;

	bool missedFrame;

	struct
	{
		SDL_AudioSpec 		spec;
		SDL_AudioDeviceID 	device;
		SDL_AudioCVT 		cvt;
	} audio;
} platform;

static inline bool crtMonitorEnabled()
{
	return platform.studio->config()->crtMonitor && platform.gpu.shader;
}

static void initSound()
{
	SDL_AudioSpec want =
	{
		.freq = 44100,
		.format = AUDIO_S16,
		.channels = 1,
		.userdata = NULL,
	};

	platform.audio.device = SDL_OpenAudioDevice(NULL, 0, &want, &platform.audio.spec, SDL_AUDIO_ALLOW_ANY_CHANGE);

	SDL_BuildAudioCVT(&platform.audio.cvt, want.format, want.channels, platform.audio.spec.freq, platform.audio.spec.format, platform.audio.spec.channels, platform.audio.spec.freq);

	if(platform.audio.cvt.needed)
	{
		platform.audio.cvt.len = platform.audio.spec.freq * sizeof(s16) / TIC_FRAMERATE;
		platform.audio.cvt.buf = SDL_malloc(platform.audio.cvt.len * platform.audio.cvt.len_mult);
	}
}

static u8* getSpritePtr(tic_tile* tiles, s32 x, s32 y)
{
	enum { SheetCols = (TIC_SPRITESHEET_SIZE / TIC_SPRITESIZE) };
	return tiles[x / TIC_SPRITESIZE + y / TIC_SPRITESIZE * SheetCols].data;
}

static u8 getSpritePixel(tic_tile* tiles, s32 x, s32 y)
{
	return tic_tool_peek4(getSpritePtr(tiles, x, y), (x % TIC_SPRITESIZE) + (y % TIC_SPRITESIZE) * TIC_SPRITESIZE);
}

static void setWindowIcon()
{
	enum{ Size = 64, TileSize = 16, ColorKey = 14, Cols = TileSize / TIC_SPRITESIZE, Scale = Size/TileSize};
	platform.studio->tic->api.clear(platform.studio->tic, 0);

	u32* pixels = SDL_malloc(Size * Size * sizeof(u32));

	const u32* pal = tic_palette_blit(&platform.studio->tic->config.palette);

	for(s32 j = 0, index = 0; j < Size; j++)
		for(s32 i = 0; i < Size; i++, index++)
		{
			u8 color = getSpritePixel(platform.studio->tic->config.bank0.tiles.data, i/Scale, j/Scale);
			pixels[index] = color == ColorKey ? 0 : pal[color];
		}

	SDL_Surface* surface = SDL_CreateRGBSurfaceFrom(pixels, Size, Size,
		sizeof(s32) * BITS_IN_BYTE, Size * sizeof(s32),
		0x000000ff, 0x0000ff00, 0x00ff0000, 0xff000000);

	SDL_SetWindowIcon(platform.window, surface);
	SDL_FreeSurface(surface);
	SDL_free(pixels);
}

static void updateGamepadParts()
{
	s32 tileSize = TIC_SPRITESIZE;
	s32 offset = 0;
	SDL_Rect rect;

	const s32 JoySize = 3;
	SDL_GetWindowSize(platform.window, &rect.w, &rect.h);

	if(rect.w < rect.h)
	{
		tileSize = rect.w / 2 / JoySize;
		offset = (rect.h * 2 - JoySize * tileSize) / 3;
	}
	else
	{
		tileSize = rect.w / 5 / JoySize;
		offset = (rect.h - JoySize * tileSize) / 2;
	}

	platform.gamepad.part.size = tileSize;
	platform.gamepad.part.axis = (SDL_Point){0, offset};
	platform.gamepad.part.a = (SDL_Point){rect.w - 2*tileSize, 2*tileSize + offset};
	platform.gamepad.part.b = (SDL_Point){rect.w - 1*tileSize, 1*tileSize + offset};
	platform.gamepad.part.x = (SDL_Point){rect.w - 3*tileSize, 1*tileSize + offset};
	platform.gamepad.part.y = (SDL_Point){rect.w - 2*tileSize, 0*tileSize + offset};
}

static void initTouchGamepad()
{
	platform.studio->tic->api.map(platform.studio->tic, &platform.studio->tic->config.bank0.map, &platform.studio->tic->config.bank0.tiles, 0, 0, TIC_MAP_SCREEN_WIDTH, TIC_MAP_SCREEN_HEIGHT, 0, 0, -1, 1);

	if(!platform.gamepad.texture)
	{		
		platform.gamepad.texture = GPU_CreateImage(TEXTURE_SIZE, TEXTURE_SIZE, STUDIO_PIXEL_FORMAT);
		GPU_SetAnchor(platform.gamepad.texture, 0, 0);
		GPU_SetImageFilter(platform.gamepad.texture, GPU_FILTER_NEAREST);
	}

	u32* data = SDL_malloc(TEXTURE_SIZE * TEXTURE_SIZE * sizeof(u32));

	if(data)
	{
		u32* out = data;

		const u8* in = platform.studio->tic->ram.vram.screen.data;
		const u8* end = in + sizeof(platform.studio->tic->ram.vram.screen);
		const u32* pal = tic_palette_blit(&platform.studio->tic->config.palette);
		const u32 Delta = ((TIC80_FULLWIDTH*sizeof(u32))/sizeof *out - TIC80_WIDTH);

		s32 col = 0;

		while(in != end)
		{
			u8 low = *in & 0x0f;
			u8 hi = (*in & 0xf0) >> TIC_PALETTE_BPP;
			*out++ = low ? *(pal + low) : 0;
			*out++ = hi ? *(pal + hi) : 0;
			in++;

			col += BITS_IN_BYTE / TIC_PALETTE_BPP;

			if (col == TIC80_WIDTH)
			{
				col = 0;
				out += Delta;
			}
		}

		GPU_UpdateImageBytes(platform.gamepad.texture, NULL, (const u8*)data, TEXTURE_SIZE * sizeof(u32));

		SDL_free(data);

		updateGamepadParts();
	}
}

static void calcTextureRect(SDL_Rect* rect)
{
	SDL_GetWindowSize(platform.window, &rect->w, &rect->h);

	if(crtMonitorEnabled())
	{
		enum{Width = TIC80_FULLWIDTH, Height = TIC80_FULLHEIGHT};

		if (rect->w * Height < rect->h * Width)
		{
			rect->x = 0;
			rect->y = 0;

			rect->h = Height * rect->w / Width;
		}
		else
		{
			s32 width = Width * rect->h / Height;

			rect->x = (rect->w - width) / 2;
			rect->y = 0;

			rect->w = width;
		}
	}
	else
	{
		enum{Width = TIC80_WIDTH, Height = TIC80_HEIGHT};

		if (rect->w * Height < rect->h * Width)
		{
			s32 discreteWidth = rect->w - rect->w % Width;
			s32 discreteHeight = Height * discreteWidth / Width;

			rect->x = (rect->w - discreteWidth) / 2;

			rect->y = rect->w > rect->h 
				? (rect->h - discreteHeight) / 2 
				: OFFSET_TOP*discreteWidth/Width;

			rect->w = discreteWidth;
			rect->h = discreteHeight;

		}
		else
		{
			s32 discreteHeight = rect->h - rect->h % Height;
			s32 discreteWidth = Width * discreteHeight / Height;

			rect->x = (rect->w - discreteWidth) / 2;
			rect->y = (rect->h - discreteHeight) / 2;

			rect->w = discreteWidth;
			rect->h = discreteHeight;
		}
	}
}

static void processMouse()
{
	s32 mx = 0, my = 0;
	s32 mb = SDL_GetMouseState(&mx, &my);

	tic80_input* input = &platform.studio->tic->ram.input;

	{
		input->mouse.x = input->mouse.y = 0;

		SDL_Rect rect = {0, 0, 0, 0};
		calcTextureRect(&rect);

		if(crtMonitorEnabled())
		{
			if(rect.w) input->mouse.x = (mx - rect.x) * TIC80_FULLWIDTH / rect.w - OFFSET_LEFT;
			if(rect.h) input->mouse.y = (my - rect.y) * TIC80_FULLHEIGHT / rect.h - OFFSET_TOP;
		}
		else
		{
			if(rect.w) input->mouse.x = (mx - rect.x) * TIC80_WIDTH / rect.w;
			if(rect.h) input->mouse.y = (my - rect.y) * TIC80_HEIGHT / rect.h;
		}
	}

	{
		input->mouse.left = mb & SDL_BUTTON_LMASK ? 1 : 0;
		input->mouse.middle = mb & SDL_BUTTON_MMASK ? 1 : 0;
		input->mouse.right = mb & SDL_BUTTON_RMASK ? 1 : 0;
	}
}

static void processKeyboard()
{
	static const u8 KeyboardCodes[] = 
	{
		#include "keycodes.c"
	};

	tic80_input* input = &platform.studio->tic->ram.input;
	input->keyboard.data = 0;

	const u8* keyboard = SDL_GetKeyboardState(NULL);

	for(s32 i = 0, c = 0; i < COUNT_OF(KeyboardCodes) && c < COUNT_OF(input->keyboard.keys); i++)
		if(keyboard[i] && KeyboardCodes[i] > tic_key_unknown)
			input->keyboard.keys[c++] = KeyboardCodes[i];
}

#if !defined(__EMSCRIPTEN__) && !defined(__MACOSX__)

static bool checkTouch(const SDL_Rect* rect, s32* x, s32* y)
{
	s32 devices = SDL_GetNumTouchDevices();
	s32 width = 0, height = 0;
	SDL_GetWindowSize(platform.window, &width, &height);

	for (s32 i = 0; i < devices; i++)
	{
		SDL_TouchID id = SDL_GetTouchDevice(i);

		// very strange, but on Android id always == 0
		//if (id)
		{
			s32 fingers = SDL_GetNumTouchFingers(id);

			if(fingers)
			{
				platform.gamepad.counter = 0;

				if (!platform.gamepad.show)
				{
					platform.gamepad.alpha = platform.studio->config()->theme.gamepad.touch.alpha;
					GPU_SetRGBA(platform.gamepad.texture, 0xff, 0xff, 0xff, platform.gamepad.alpha);
					platform.gamepad.show = true;
					return false;
				}
			}

			for (s32 f = 0; f < fingers; f++)
			{
				SDL_Finger* finger = SDL_GetTouchFinger(id, f);

				if (finger && finger->pressure > 0.0f)
				{
					SDL_Point point = { (s32)(finger->x * width), (s32)(finger->y * height) };
					if (SDL_PointInRect(&point, rect))
					{
						*x = point.x;
						*y = point.y;
						return true;
					}
				}
			}
		}
	}

	return false;
}

static void processTouchGamepad()
{
	platform.gamepad.touch.data = 0;

	const s32 size = platform.gamepad.part.size;
	s32 x = 0, y = 0;

	{
		SDL_Rect axis = {platform.gamepad.part.axis.x, platform.gamepad.part.axis.y, size*3, size*3};

		if(checkTouch(&axis, &x, &y))
		{
			x -= axis.x;
			y -= axis.y;

			s32 xt = x / size;
			s32 yt = y / size;

			if(yt == 0) platform.gamepad.touch.first.up = true;
			else if(yt == 2) platform.gamepad.touch.first.down = true;

			if(xt == 0) platform.gamepad.touch.first.left = true;
			else if(xt == 2) platform.gamepad.touch.first.right = true;

			if(xt == 1 && yt == 1)
			{
				xt = (x - size)/(size/3);
				yt = (y - size)/(size/3);

				if(yt == 0) platform.gamepad.touch.first.up = true;
				else if(yt == 2) platform.gamepad.touch.first.down = true;

				if(xt == 0) platform.gamepad.touch.first.left = true;
				else if(xt == 2) platform.gamepad.touch.first.right = true;
			}
		}
	}

	{
		SDL_Rect a = {platform.gamepad.part.a.x, platform.gamepad.part.a.y, size, size};
		if(checkTouch(&a, &x, &y)) platform.gamepad.touch.first.a = true;
	}

	{
		SDL_Rect b = {platform.gamepad.part.b.x, platform.gamepad.part.b.y, size, size};
		if(checkTouch(&b, &x, &y)) platform.gamepad.touch.first.b = true;
	}

	{
		SDL_Rect xb = {platform.gamepad.part.x.x, platform.gamepad.part.x.y, size, size};
		if(checkTouch(&xb, &x, &y)) platform.gamepad.touch.first.x = true;
	}

	{
		SDL_Rect yb = {platform.gamepad.part.y.x, platform.gamepad.part.y.y, size, size};
		if(checkTouch(&yb, &x, &y)) platform.gamepad.touch.first.y = true;
	}
}

#endif

static s32 getAxisMask(SDL_Joystick* joystick)
{
	s32 mask = 0;

	s32 axesCount = SDL_JoystickNumAxes(joystick);

	for (s32 a = 0; a < axesCount; a++)
	{
		s32 axe = SDL_JoystickGetAxis(joystick, a);

		if (axe)
		{
			if (a == 0)
			{
				if (axe > 16384) mask |= SDL_HAT_RIGHT;
				else if(axe < -16384) mask |= SDL_HAT_LEFT;
			}
			else if (a == 1)
			{
				if (axe > 16384) mask |= SDL_HAT_DOWN;
				else if (axe < -16384) mask |= SDL_HAT_UP;
			}
		}
	}

	return mask;
}

static s32 getJoystickHatMask(s32 hat)
{
	tic80_gamepads gamepad;
	gamepad.data = 0;

	gamepad.first.up = hat & SDL_HAT_UP;
	gamepad.first.down = hat & SDL_HAT_DOWN;
	gamepad.first.left = hat & SDL_HAT_LEFT;
	gamepad.first.right = hat & SDL_HAT_RIGHT;

	return gamepad.data;
}

static void processJoysticks()
{
	platform.gamepad.joystick.data = 0;
	s32 index = 0;

	for(s32 i = 0; i < COUNT_OF(platform.gamepad.ports); i++)
	{
		SDL_Joystick* joystick = platform.gamepad.ports[i];

		if(joystick && SDL_JoystickGetAttached(joystick))
		{
			tic80_gamepad* gamepad = NULL;

			switch(index)
			{
			case 0: gamepad = &platform.gamepad.joystick.first; break;
			case 1: gamepad = &platform.gamepad.joystick.second; break;
			case 2: gamepad = &platform.gamepad.joystick.third; break;
			case 3: gamepad = &platform.gamepad.joystick.fourth; break;
			}

			if(gamepad)
			{
				gamepad->data |= getJoystickHatMask(getAxisMask(joystick));

				for (s32 h = 0; h < SDL_JoystickNumHats(joystick); h++)
					gamepad->data |= getJoystickHatMask(SDL_JoystickGetHat(joystick, h));

				s32 numButtons = SDL_JoystickNumButtons(joystick);
				if(numButtons >= 2)
				{
					gamepad->a = SDL_JoystickGetButton(joystick, 0);
					gamepad->b = SDL_JoystickGetButton(joystick, 1);

					if(numButtons >= 4)
					{
						gamepad->x = SDL_JoystickGetButton(joystick, 2);
						gamepad->y = SDL_JoystickGetButton(joystick, 3);

						for(s32 i = 5; i < numButtons; i++)
						{
							s32 back = SDL_JoystickGetButton(joystick, i);

							if(back)
							{
								tic_mem* tic = platform.studio->tic;

								for(s32 i = 0; i < TIC80_KEY_BUFFER; i++)
								{
									if(!tic->ram.input.keyboard.keys[i])
									{
										tic->ram.input.keyboard.keys[i] = tic_key_escape;
										break;
									}
								}
							}
						}
					}
				}

				index++;
			}
		}
	}
}

static void processGamepad()
{
#if !defined(__EMSCRIPTEN__) && !defined(__MACOSX__)
	processTouchGamepad();
#endif
	processJoysticks();
	
	{
		platform.studio->tic->ram.input.gamepads.data = 0;

		platform.studio->tic->ram.input.gamepads.data |= platform.gamepad.touch.data;
		platform.studio->tic->ram.input.gamepads.data |= platform.gamepad.joystick.data;
	}
}

static void pollEvent()
{
	tic80_input* input = &platform.studio->tic->ram.input;

	{
		input->mouse.btns = 0;
	}

	SDL_Event event;

	while(SDL_PollEvent(&event))
	{
		switch(event.type)
		{
		case SDL_MOUSEWHEEL:
			{
				input->mouse.scrollx = event.wheel.x;
				input->mouse.scrolly = event.wheel.y;				
			}
			break;
		case SDL_JOYDEVICEADDED:
			{
				s32 id = event.jdevice.which;

				if (id < TIC_GAMEPADS)
				{
					if(platform.gamepad.ports[id])
						SDL_JoystickClose(platform.gamepad.ports[id]);

					platform.gamepad.ports[id] = SDL_JoystickOpen(id);
				}
			}
			break;

		case SDL_JOYDEVICEREMOVED:
			{
				s32 id = event.jdevice.which;

				if (id < TIC_GAMEPADS && platform.gamepad.ports[id])
				{
					SDL_JoystickClose(platform.gamepad.ports[id]);
					platform.gamepad.ports[id] = NULL;
				}
			}
			break;
		case SDL_WINDOWEVENT:
			switch(event.window.event)
			{
			case SDL_WINDOWEVENT_RESIZED: 
			{
				s32 w = 0, h = 0;
				SDL_GetWindowSize(platform.window, &w, &h);
				GPU_SetWindowResolution(w, h);

				updateGamepadParts(); break;
			}
			case SDL_WINDOWEVENT_FOCUS_GAINED: platform.studio->updateProject(); break;
			}
			break;
		case SDL_QUIT:
			platform.studio->exit();
			break;
		default:
			break;
		}
	}

	processMouse();
	processKeyboard();
	processGamepad();
}

static void blitGpuTexture(GPU_Target* screen, GPU_Image* texture)
{
	SDL_Rect rect = {0, 0, 0, 0};
	calcTextureRect(&rect);

	enum {Header = OFFSET_TOP, Top = OFFSET_TOP, Left = OFFSET_LEFT};

	s32 width = 0;
	SDL_GetWindowSize(platform.window, &width, NULL);

	{
		GPU_Rect srcRect = {0, 0, TIC80_FULLWIDTH, Header};
		GPU_Rect dstRect = {0, 0, width, rect.y};
		GPU_BlitScale(texture, &srcRect, screen, dstRect.x, dstRect.y, dstRect.w / srcRect.w, dstRect.h / srcRect.h);
	}

	{
		GPU_Rect srcRect = {0, TIC80_FULLHEIGHT - Header, TIC80_FULLWIDTH, Header};
		GPU_Rect dstRect = {0, rect.y + rect.h, width, rect.y};
		GPU_BlitScale(texture, &srcRect, screen, dstRect.x, dstRect.y, dstRect.w / srcRect.w, dstRect.h / srcRect.h);
	}

	{
		GPU_Rect srcRect = {0, Header, Left, TIC80_HEIGHT};
		GPU_Rect dstRect = {0, rect.y, width, rect.h};
		GPU_BlitScale(texture, &srcRect, screen, dstRect.x, dstRect.y, dstRect.w / srcRect.w, dstRect.h / srcRect.h);
	}

	{
		GPU_Rect srcRect = {Left, Top, TIC80_WIDTH, TIC80_HEIGHT};
		GPU_Rect dstRect = {rect.x, rect.y, rect.w, rect.h};
		GPU_BlitScale(texture, &srcRect, screen, dstRect.x, dstRect.y, dstRect.w / srcRect.w, dstRect.h / srcRect.h);
	}
}

static void blitSound()
{
	tic_mem* tic = platform.studio->tic;

	SDL_PauseAudioDevice(platform.audio.device, 0);
	
	if(platform.audio.cvt.needed)
	{
		SDL_memcpy(platform.audio.cvt.buf, tic->samples.buffer, tic->samples.size);
		SDL_ConvertAudio(&platform.audio.cvt);
		SDL_QueueAudio(platform.audio.device, platform.audio.cvt.buf, platform.audio.cvt.len_cvt);
	}
	else SDL_QueueAudio(platform.audio.device, tic->samples.buffer, tic->samples.size);
}

static void renderGamepad()
{
	if(platform.gamepad.show || platform.gamepad.alpha); else return;

	const s32 tileSize = platform.gamepad.part.size;
	const SDL_Point axis = platform.gamepad.part.axis;
	typedef struct { bool press; s32 x; s32 y;} Tile;
	const Tile Tiles[] =
	{
		{platform.studio->tic->ram.input.gamepads.first.up, 	axis.x + 1*tileSize, axis.y + 0*tileSize},
		{platform.studio->tic->ram.input.gamepads.first.down, 	axis.x + 1*tileSize, axis.y + 2*tileSize},
		{platform.studio->tic->ram.input.gamepads.first.left, 	axis.x + 0*tileSize, axis.y + 1*tileSize},
		{platform.studio->tic->ram.input.gamepads.first.right, 	axis.x + 2*tileSize, axis.y + 1*tileSize},

		{platform.studio->tic->ram.input.gamepads.first.a, 		platform.gamepad.part.a.x, platform.gamepad.part.a.y},
		{platform.studio->tic->ram.input.gamepads.first.b, 		platform.gamepad.part.b.x, platform.gamepad.part.b.y},
		{platform.studio->tic->ram.input.gamepads.first.x, 		platform.gamepad.part.x.x, platform.gamepad.part.x.y},
		{platform.studio->tic->ram.input.gamepads.first.y, 		platform.gamepad.part.y.x, platform.gamepad.part.y.y},
	};

	enum {ButtonsCount = 8};

	for(s32 i = 0; i < COUNT_OF(Tiles); i++)
	{
		const Tile* tile = Tiles + i;
		GPU_Rect src = {(tile->press ? ButtonsCount + i : i) * TIC_SPRITESIZE, 0, TIC_SPRITESIZE, TIC_SPRITESIZE};
		GPU_Rect dest = {tile->x, tile->y, tileSize, tileSize};

		GPU_BlitScale(platform.gamepad.texture, &src, platform.gpu.screen, dest.x, dest.y,
			(float)dest.w / TIC_SPRITESIZE, (float)dest.h / TIC_SPRITESIZE);
	}

	if(!platform.gamepad.show && platform.gamepad.alpha)
	{
		enum {Step = 3};

		if(platform.gamepad.alpha - Step >= 0) platform.gamepad.alpha -= Step;
		else platform.gamepad.alpha = 0;

		GPU_SetRGBA(platform.gamepad.texture, 0xff, 0xff, 0xff, platform.gamepad.alpha);
	}

	platform.gamepad.counter = platform.gamepad.touch.data ? 0 : platform.gamepad.counter+1;

	// wait 5 seconds and hide touch gamepad
	if(platform.gamepad.counter >= 5 * TIC_FRAMERATE)
		platform.gamepad.show = false;
}

static void blitCursor(const u8* in)
{
	if(!platform.mouse.texture)
	{
		platform.mouse.texture = GPU_CreateImage(TIC_SPRITESIZE, TIC_SPRITESIZE, STUDIO_PIXEL_FORMAT);
		GPU_SetAnchor(platform.mouse.texture, 0, 0);
		GPU_SetImageFilter(platform.mouse.texture, GPU_FILTER_NEAREST);
	}

	if(platform.mouse.src != in)
	{
		platform.mouse.src = in;

		const u8* end = in + sizeof(tic_tile);
		const u32* pal = tic_palette_blit(&platform.studio->tic->ram.vram.palette);
		static u32 data[TIC_SPRITESIZE*TIC_SPRITESIZE];
		u32* out = data;

		while(in != end)
		{
			u8 low = *in & 0x0f;
			u8 hi = (*in & 0xf0) >> TIC_PALETTE_BPP;
			*out++ = low ? *(pal + low) : 0;
			*out++ = hi ? *(pal + hi) : 0;

			in++;
		}

		GPU_UpdateImageBytes(platform.mouse.texture, NULL, (const u8*)data, TIC_SPRITESIZE * sizeof(u32));
	}

	SDL_Rect rect = {0, 0, 0, 0};
	calcTextureRect(&rect);
	s32 scale = rect.w / TIC80_WIDTH;

	s32 mx, my;
	SDL_GetMouseState(&mx, &my);

	if(platform.studio->config()->theme.cursor.pixelPerfect)
	{
		mx -= (mx - rect.x) % scale;
		my -= (my - rect.y) % scale;
	}

	if(SDL_GetWindowFlags(platform.window) & SDL_WINDOW_MOUSE_FOCUS)
		GPU_BlitScale(platform.mouse.texture, NULL, platform.gpu.screen, mx, my, (float)scale, (float)scale);
}

static void renderCursor()
{
	if(platform.studio->tic->ram.vram.vars.cursor.system)
	{
		switch(platform.studio->tic->ram.vram.vars.cursor.sprite)
		{
		case tic_cursor_hand: 
			{
				if(platform.studio->config()->theme.cursor.hand >= 0)
				{
					SDL_ShowCursor(SDL_DISABLE);
					blitCursor(platform.studio->tic->config.bank0.tiles.data[platform.studio->config()->theme.cursor.hand].data);
				}
				else
				{
					SDL_ShowCursor(SDL_ENABLE);
					SDL_SetCursor(SDL_CreateSystemCursor(SDL_SYSTEM_CURSOR_HAND));
				}
			}
			break;
		case tic_cursor_ibeam:
			{
				if(platform.studio->config()->theme.cursor.ibeam >= 0)
				{
					SDL_ShowCursor(SDL_DISABLE);
					blitCursor(platform.studio->tic->config.bank0.tiles.data[platform.studio->config()->theme.cursor.ibeam].data);
				}
				else
				{
					SDL_ShowCursor(SDL_ENABLE);
					SDL_SetCursor(SDL_CreateSystemCursor(SDL_SYSTEM_CURSOR_IBEAM));
				}
			}
			break;
		default:
			{
				if(platform.studio->config()->theme.cursor.arrow >= 0)
				{
					SDL_ShowCursor(SDL_DISABLE);
					blitCursor(platform.studio->tic->config.bank0.tiles.data[platform.studio->config()->theme.cursor.arrow].data);
				}
				else
				{
					SDL_ShowCursor(SDL_ENABLE);
					SDL_SetCursor(SDL_CreateSystemCursor(SDL_SYSTEM_CURSOR_ARROW));
				}
			}
		}
	}
	else
	{
		SDL_ShowCursor(SDL_DISABLE);
		blitCursor(platform.studio->tic->ram.sprites.data[platform.studio->tic->ram.vram.vars.cursor.sprite].data);
	}

// 	if(platform.mode == TIC_RUN_MODE && !platform.studio.tic->input.mouse)
// 	{
// 		SDL_ShowCursor(SDL_DISABLE);
// 		return;
// 	}
}

static const char* getAppFolder()
{
	static char appFolder[FILENAME_MAX];

#if defined(__EMSCRIPTEN__)

		strcpy(appFolder, "/" TIC_PACKAGE "/" TIC_NAME "/");

#elif defined(__ANDROID__)

		strcpy(appFolder, SDL_AndroidGetExternalStoragePath());
		const char AppFolder[] = "/" TIC_NAME "/";
		strcat(appFolder, AppFolder);
		mkdir(appFolder, 0700);

#else

		char* path = SDL_GetPrefPath(TIC_PACKAGE, TIC_NAME);
		strcpy(appFolder, path);
		SDL_free(path);

#endif

	return appFolder;
}

static void setClipboardText(const char* text)
{
	SDL_SetClipboardText(text);
}

static bool hasClipboardText()
{
	return SDL_HasClipboardText();
}

static char* getClipboardText()
{
	return SDL_GetClipboardText();
}

static u64 getPerformanceCounter()
{
	return SDL_GetPerformanceCounter();
}

static u64 getPerformanceFrequency()
{
	return SDL_GetPerformanceFrequency();
}

static void goFullscreen()
{
	GPU_SetFullscreen(GPU_GetFullscreen() ? false : true, true);
}

static void showMessageBox(const char* title, const char* message)
{
	SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_WARNING, title, message, NULL);
}

static void setWindowTitle(const char* title)
{
	SDL_SetWindowTitle(platform.window, title);
}

#if defined(__WINDOWS__) || defined(__LINUX__) || defined(__MACOSX__)

static void openSystemPath(const char* path)
{
	char command[FILENAME_MAX];

#if defined(__WINDOWS__)

	sprintf(command, "explorer \"%s\"", path);

	wchar_t wcommand[FILENAME_MAX];
	mbstowcs(wcommand, command, FILENAME_MAX);

	_wsystem(wcommand);

#elif defined(__LINUX__)

	sprintf(command, "xdg-open \"%s\"", path);
	system(command);

#elif defined(__MACOSX__)

	sprintf(command, "open \"%s\"", path);
	system(command);

#endif
}

#else

static void openSystemPath(const char* path) {}

#endif

static void* getUrlRequest(const char* url, s32* size)
{
	return netGetRequest(platform.net, url, size);
}

static void preseed()
{
#if defined(__MACOSX__)
	srandom(time(NULL));
	random();
#else
	srand(time(NULL));
	rand();
#endif
}

static void loadCrtShader()
{
	static const char* VertexShader = "#version 100\n\
		precision highp float;\n\
		precision mediump int;\n\
		attribute vec2 gpu_Vertex;\n\
		attribute vec2 gpu_TexCoord;\n\
		attribute mediump vec4 gpu_Color;\n\
		uniform mat4 gpu_ModelViewProjectionMatrix;\n\
		varying mediump vec4 color;\n\
		varying vec2 texCoord;\n\
		void main(void)\n\
		{\n\
			color = gpu_Color;\n\
			texCoord = vec2(gpu_TexCoord);\n\
			gl_Position = gpu_ModelViewProjectionMatrix * vec4(gpu_Vertex, 0.0, 1.0);\n\
		}";

	u32 vertex = GPU_CompileShader(GPU_VERTEX_SHADER, VertexShader);
	
	if(!vertex)
	{
		char msg[1024];
		sprintf(msg, "Failed to load vertex shader: %s\n", GPU_GetShaderMessage());
		showMessageBox("Error", msg);
		return;
	}

	u32 fragment = GPU_CompileShader(GPU_PIXEL_SHADER, platform.studio->config()->crtShader);
	
	if(!fragment)
	{
		char msg[1024];
		sprintf(msg, "Failed to load fragment shader: %s\n", GPU_GetShaderMessage());
		showMessageBox("Error", msg);
		return;
	}
	
	if(platform.gpu.shader)
		GPU_FreeShaderProgram(platform.gpu.shader);

	platform.gpu.shader = GPU_LinkShaders(vertex, fragment);
	
	if(platform.gpu.shader)
	{
		platform.gpu.block = GPU_LoadShaderBlock(platform.gpu.shader, "gpu_Vertex", "gpu_TexCoord", "gpu_Color", "gpu_ModelViewProjectionMatrix");
		GPU_ActivateShaderProgram(platform.gpu.shader, &platform.gpu.block);
	}
	else
	{
		char msg[1024];
		sprintf(msg, "Failed to link shader program: %s\n", GPU_GetShaderMessage());
		showMessageBox("Error", msg);
	}
}

static void updateConfig()
{
	if(platform.gpu.screen)
	{
		initTouchGamepad();
		loadCrtShader();
	}
}

static System systemInterface = 
{
	.setClipboardText = setClipboardText,
	.hasClipboardText = hasClipboardText,
	.getClipboardText = getClipboardText,
	.getPerformanceCounter = getPerformanceCounter,
	.getPerformanceFrequency = getPerformanceFrequency,

	.getUrlRequest = getUrlRequest,

	.fileDialogLoad = file_dialog_load,
	.fileDialogSave = file_dialog_save,

	.goFullscreen = goFullscreen,
	.showMessageBox = showMessageBox,
	.setWindowTitle = setWindowTitle,

	.openSystemPath = openSystemPath,
	.preseed = preseed,
	.poll = pollEvent,
	.updateConfig = updateConfig,
};

static void gpuTick()
{
	tic_mem* tic = platform.studio->tic;

	pollEvent();

	if(platform.studio->quit)
	{
#if defined __EMSCRIPTEN__
		emscripten_cancel_main_loop();
#endif
		return;
	}

	GPU_Clear(platform.gpu.screen);

	{
		platform.studio->tick();

		GPU_UpdateImageBytes(platform.gpu.texture, NULL, (const u8*)tic->screen, TIC80_FULLWIDTH * sizeof(u32));

		{
			if(crtMonitorEnabled())
			{
				SDL_Rect rect = {0, 0, 0, 0};
				calcTextureRect(&rect);

				GPU_ActivateShaderProgram(platform.gpu.shader, &platform.gpu.block);

				GPU_SetUniformf(GPU_GetUniformLocation(platform.gpu.shader, "trg_x"), rect.x);
				GPU_SetUniformf(GPU_GetUniformLocation(platform.gpu.shader, "trg_y"), rect.y);
				GPU_SetUniformf(GPU_GetUniformLocation(platform.gpu.shader, "trg_w"), rect.w);
				GPU_SetUniformf(GPU_GetUniformLocation(platform.gpu.shader, "trg_h"), rect.h);

				{
					s32 w, h;
					SDL_GetWindowSize(platform.window, &w, &h);
					GPU_SetUniformf(GPU_GetUniformLocation(platform.gpu.shader, "scr_w"), w);
					GPU_SetUniformf(GPU_GetUniformLocation(platform.gpu.shader, "scr_h"), h);
				}

				GPU_BlitScale(platform.gpu.texture, NULL, platform.gpu.screen, rect.x, rect.y, 
					(float)rect.w / TIC80_FULLWIDTH, (float)rect.h / TIC80_FULLHEIGHT);

				GPU_DeactivateShaderProgram();
			}
			else
			{
				GPU_DeactivateShaderProgram();
				blitGpuTexture(platform.gpu.screen, platform.gpu.texture);
			}
		}

		renderCursor();
		renderGamepad();
	}

	GPU_Flip(platform.gpu.screen);

	blitSound();
}

#if defined(__EMSCRIPTEN__)

static void emsGpuTick()
{
	static double nextTick = -1.0;

	platform.missedFrame = false;

	if(nextTick < 0.0)
		nextTick = emscripten_get_now();

	nextTick += 1000.0/TIC_FRAMERATE;
	gpuTick();
	double delay = nextTick - emscripten_get_now();

	if(delay < 0.0)
	{
		nextTick -= delay;
		platform.missedFrame = true;
	}
	else
		emscripten_set_main_loop_timing(EM_TIMING_SETTIMEOUT, delay);
}

#endif

static s32 start(s32 argc, char **argv, const char* folder)
{
	SDL_Init(SDL_INIT_VIDEO | SDL_INIT_AUDIO | SDL_INIT_JOYSTICK);

	initSound();

	platform.net = createNet();

	platform.studio = studioInit(argc, argv, platform.audio.spec.freq, folder, &systemInterface);

	enum{Width = TIC80_FULLWIDTH * STUDIO_UI_SCALE, Height = TIC80_FULLHEIGHT * STUDIO_UI_SCALE};

	platform.window = SDL_CreateWindow( TIC_TITLE, SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED,
		Width, Height, SDL_WINDOW_SHOWN | SDL_WINDOW_RESIZABLE| SDL_WINDOW_OPENGL);

	setWindowIcon();

	GPU_SetInitWindow(SDL_GetWindowID(platform.window));

	platform.gpu.screen = GPU_Init(Width, Height, GPU_INIT_DISABLE_VSYNC);

	initTouchGamepad();

	platform.gpu.texture = GPU_CreateImage(TIC80_FULLWIDTH, TIC80_FULLHEIGHT, STUDIO_PIXEL_FORMAT);
	GPU_SetAnchor(platform.gpu.texture, 0, 0);
	GPU_SetImageFilter(platform.gpu.texture, GPU_FILTER_NEAREST);

	loadCrtShader();

#if defined(__EMSCRIPTEN__)

	emscripten_set_main_loop(emsGpuTick, 0, 1);

#else
	{
		u64 nextTick = SDL_GetPerformanceCounter();
		const u64 Delta = SDL_GetPerformanceFrequency() / TIC_FRAMERATE;

		while (!platform.studio->quit)
		{
			platform.missedFrame = false;

			nextTick += Delta;
			
			gpuTick();

			{
				s64 delay = nextTick - SDL_GetPerformanceCounter();

				if(delay < 0)
				{
					nextTick -= delay;
					platform.missedFrame = true;
				}
				else SDL_Delay((u32)(delay * 1000 / SDL_GetPerformanceFrequency()));
			}
		}
	}

#endif

	platform.studio->close();

	closeNet(platform.net);

	if(platform.audio.cvt.buf)
		SDL_free(platform.audio.cvt.buf);

	if(platform.gpu.shader)
		GPU_FreeShaderProgram(platform.gpu.shader);

	GPU_FreeImage(platform.gpu.texture);

	if(platform.gamepad.texture)
		GPU_FreeImage(platform.gamepad.texture);

	if(platform.mouse.texture)
		GPU_FreeImage(platform.mouse.texture);

	SDL_DestroyWindow(platform.window);
	SDL_CloseAudioDevice(platform.audio.device);

	GPU_Quit();

	return 0;
}

#if defined(__EMSCRIPTEN__)

#define DEFAULT_CART "cart.tic"

static struct
{
	s32 argc;
	char **argv;
	const char* folder;
} startVars;

static void onEmscriptenWget(const char* file)
{
	startVars.argv[1] = DEFAULT_CART;
	start(startVars.argc, startVars.argv, startVars.folder);
}

static void onEmscriptenWgetError(const char* error) {}

static void emsStart(s32 argc, char **argv, const char* folder)
{
	if(argc == 2)
	{
		startVars.argc = argc;
		startVars.argv = argv;
		startVars.folder = folder;

		emscripten_async_wget(argv[1], DEFAULT_CART, onEmscriptenWget, onEmscriptenWgetError);
	}
	else start(argc, argv, folder);
}

#endif

s32 main(s32 argc, char **argv)
{
	const char* folder = getAppFolder();

#if defined(__EMSCRIPTEN__)

	EM_ASM_
	(
		{
			var dir = "";
			Module.Pointer_stringify($0).split("/").forEach(function(val)
			{
				if(val.length)
				{
					dir += "/" + val;
					FS.mkdir(dir);
				}
			});
			
			FS.mount(IDBFS, {}, dir);
			FS.syncfs(true, function()
			{
				Runtime.dynCall('viii', $1, [$2, $3, $0]);
			});

		}, folder, emsStart, argc, argv
	);

#else

	return start(argc, argv, folder);
	
#endif
}