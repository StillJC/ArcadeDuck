#include "ReShade.fxh"

// CRT-NewPixie (Curvature-Free)
// by Mattias Gustavsson
// adapted for slang by hunterk
// curvature/blur borders removed + UI toggles
// by Conkwer (SSF/Satpixie optimized)
// Gamma fix applied - Natural Vision now works correctly
// Chromatic Aberration toggle fixed
// BACKPORTED: Aspect-aware vignette + Advanced shadow mask from Satpixie and Albatross forks
// Universal stretch and position controls added
// NTSC color adjustment added (hardcoded: saturation=1.05, hue=-0.08)
// Gamma slider works for ALL color modes
// version 20260120

uniform float acc_modulate <
	ui_type = "drag";
	ui_min = 0.0; ui_max = 1.0; ui_step = 0.01;
	ui_label = "Accumulate Modulation";
> = 0.65;

uniform int color_mode <
    ui_type = "combo";
    ui_label = "Color Mode";
    ui_items = "Natural (Neutral)\0Original (Green Tint)\0NTSC Composite\0";
    ui_tooltip = "Color processing mode";
> = 0; // Default to Natural

// NTSC parameters hardcoded for simpler UI
// Saturation: 1.05 (slightly boosted colors)
// Hue Shift: -0.08 (warmer composite look)
// Edit shader source to change these values

uniform float gamma <
	ui_type = "drag"; ui_min = 1.8; ui_max = 2.6; ui_step = 0.1;
	ui_label = "Gamma";
	ui_tooltip = "Gamma correction. For Green Tint, use gamma ~2.2";
> = 2.4;

uniform bool ghosting_on <
	ui_type = "boolean";
	ui_label = "Ghosting";
> = true;

uniform bool chroma_on <
	ui_type = "boolean";
	ui_label = "Chromatic Aberration";
> = true;

uniform bool vignette_on <
	ui_type = "boolean";
	ui_label = "Vignette";
> = true;

uniform int FrameAspectMode <
    ui_type = "combo";
    ui_label = "Frame Aspect Ratio";
    ui_items = "Wide\0Pillarbox\0";
> = 1;

uniform bool wiggle_toggle <
	ui_type = "boolean";
	ui_label = "Interference";
> = false;

uniform bool scanroll <
	ui_type = "boolean";
	ui_label = "Rolling Scanlines";
> = false;  // Disabled by default

uniform float blur_x <
	ui_type = "drag"; ui_min = 0.0; ui_max = 5.0; ui_step = 0.25;
	ui_label = "Horizontal Blur";
> = 0.0;

uniform float blur_y <
	ui_type = "drag"; ui_min = 0.0; ui_max = 5.0; ui_step = 0.25;
	ui_label = "Vertical Blur";
> = 0.0;

// Consolidated CRT mode parameter
uniform int CRTMode <
    ui_type = "combo";
    ui_label = "CRT Emulation";
    ui_items = "Off (No CRT Effects)\0Off (Brightness Matched)\0Scanlines Only\0Shadow Mask (RGB Stripes)\0Aperture Grille (Brightness Lines)\0";
    ui_tooltip = "Choose CRT display type simulation";
> = 2; // Default to Scanlines Only

// Master toggle for geometry controls
uniform bool enable_geometry_controls <
    ui_type = "boolean";
    ui_label = "Enable Geometry Controls";
> = false;

// Universal image stretch and position controls (works for all systems)
uniform int stretch_vertical <
    ui_type = "drag"; ui_min = 80; ui_max = 130; ui_step = 1;
    ui_label = "Vertical Stretch (%)";
    ui_tooltip = "Stretch image vertically. 100 = no stretch, >100 = taller, <100 = shorter";
> = 100;

uniform int stretch_horizontal <
    ui_type = "drag"; ui_min = 80; ui_max = 130; ui_step = 1;
    ui_label = "Horizontal Stretch (%)";
    ui_tooltip = "Stretch image horizontally. 100 = no stretch, >100 = wider, <100 = narrower";
> = 100;

uniform int offset_horizontal <
    ui_type = "drag"; ui_min = -200; ui_max = 200; ui_step = 1;
    ui_label = "Horizontal Position";
    ui_tooltip = "Shift image left/right. 0 = centered, negative = left, positive = right";
> = 0;

uniform int offset_vertical <
    ui_type = "drag"; ui_min = -200; ui_max = 200; ui_step = 1;
    ui_label = "Vertical Position";
    ui_tooltip = "Shift image up/down. 0 = centered, negative = up, positive = down";
> = 0;


// Textures & Passes (unchanged)
texture2D tAccTex { Width = BUFFER_WIDTH; Height = BUFFER_HEIGHT; Format = RGBA8; };
sampler sAccTex { Texture=tAccTex; };

texture GaussianBlurTex { Width = BUFFER_WIDTH; Height = BUFFER_HEIGHT; Format = RGBA8; };
sampler GaussianBlurSampler { Texture = GaussianBlurTex; };

uniform int FCount < source = "framecount"; >;

// PASS 1-3 (unchanged)
float3 PrevColor(float4 pos : SV_Position, float2 uv : TEXCOORD0) : SV_Target
{
	return tex2D(ReShade::BackBuffer, uv).rgb;
}

float4 PS_NewPixie_Accum(float4 pos : SV_Position, float2 uv : TEXCOORD0) : SV_Target
{
   float4 a = tex2D(sAccTex, uv.xy) * float4(acc_modulate,acc_modulate,acc_modulate,acc_modulate);
   float4 b = tex2D(ReShade::BackBuffer, uv.xy);
   return max( a, b * 0.96 );
}

float4 PS_NewPixie_Blur(float4 pos : SV_Position, float2 uv_tx : TEXCOORD0) : SV_Target
{
   float2 blur = float2(blur_x, blur_y) * ReShade::PixelSize.xy;
   float2 uv = uv_tx.xy;
   float4 sum = tex2D( ReShade::BackBuffer, uv ) * 0.2270270270;
   sum += tex2D(ReShade::BackBuffer, float2( uv.x - 4.0 * blur.x, uv.y - 4.0 * blur.y ) ) * 0.0162162162;
   sum += tex2D(ReShade::BackBuffer, float2( uv.x - 3.0 * blur.x, uv.y - 3.0 * blur.y ) ) * 0.0540540541;
   sum += tex2D(ReShade::BackBuffer, float2( uv.x - 2.0 * blur.x, uv.y - 2.0 * blur.y ) ) * 0.1216216216;
   sum += tex2D(ReShade::BackBuffer, float2( uv.x - 1.0 * blur.x, uv.y - 1.0 * blur.y ) ) * 0.1945945946;
   sum += tex2D(ReShade::BackBuffer, float2( uv.x + 1.0 * blur.x, uv.y + 1.0 * blur.y ) ) * 0.1945945946;
   sum += tex2D(ReShade::BackBuffer, float2( uv.x + 2.0 * blur.x, uv.y + 2.0 * blur.y ) ) * 0.1216216216;
   sum += tex2D(ReShade::BackBuffer, float2( uv.x + 3.0 * blur.x, uv.y + 3.0 * blur.y ) ) * 0.0540540541;
   sum += tex2D(ReShade::BackBuffer, float2( uv.x + 4.0 * blur.x, uv.y + 4.0 * blur.y ) ) * 0.0162162162;
   return sum;
}

float3 tsample( sampler samp, float2 tc, float offs, float2 resolution )
{
    // Apply geometry controls if enabled
    if (enable_geometry_controls) {
        // Convert integer offset to UV space
        float h_offset = float(offset_horizontal) / 555.0;
        float v_offset = float(offset_vertical) / 555.0;
        
        // Apply position offset first (shift the image)
        tc.x += h_offset;
        tc.y += v_offset;
        
        // Convert integer stretch (100 = 1.0, 110 = 1.1, 90 = 0.9)
        float stretch_h = float(stretch_horizontal) / 100.0;
        float stretch_v = float(stretch_vertical) / 100.0;
        
        // Apply stretch from center
        tc.x = 0.5 + ((tc.x - 0.5) / stretch_h);
        tc.y = 0.5 + ((tc.y - 0.5) / stretch_v);
    }
    
    // Return black for out-of-bounds sampling (FIX for DuckStation artifacts)
    if (tc.x < 0.0 || tc.x > 1.0 || tc.y < 0.0 || tc.y > 1.0) {
        return float3(0.0, 0.0, 0.0);
    }
    
    // FIXED: No curvature scaling or border blur
    float3 s = pow( abs( tex2D( samp, float2( tc.x, 1.0-tc.y ) ).rgb), float3( 2.2,2.2,2.2 ) );
    return s*float3(1.25,1.25,1.25);
}

		
float3 filmic( float3 LinearColor )
{
	float3 x = max( float3(0.0,0.0,0.0), LinearColor-float3(0.004,0.004,0.004));
    return (x*(6.2*x+0.5))/(x*(6.2*x+1.7)+0.06);
}

float rand(float2 co){ return frac(sin(dot(co.xy ,float2(12.9898,78.233))) * 43758.5453); }
    
#define resolution ReShade::ScreenSize.xy
#define mod(x,y) (x-y*floor(x/y))

float4 PS_NewPixie_Final(float4 pos: SV_Position, float2 uv_tx : TEXCOORD0) : SV_Target
{
   float time = mod(FCount, 849.0) * 36.0;
    float2 uv = uv_tx.xy;
	uv.y = 1.0 - uv_tx.y;
    
    // STRAIGHT UV (no curve distortion!)
    float2 scuv = uv;
	
    uv = scuv;
		
    /* Main color, Bleed (chroma toggleable) */
    float3 col;
	
    float x = wiggle_toggle* sin(0.1*time+uv.y*13.0)*sin(0.23*time+uv.y*19.0)*sin(0.3+0.11*time+uv.y*23.0)*0.0012;
    float o =sin(uv_tx.y*1.5)/resolution.x;
    x+=o*0.25;
    
    time = float(mod(FCount, 640) * 1); 
    
    float chroma_offs_x = chroma_on ? 0.0015 : 0.0;
    float chroma_offs_y = chroma_on ? 0.0011 : 0.0;
    
	col.r = tsample(GaussianBlurSampler,float2(x+scuv.x+0.0009,scuv.y+0.0009),resolution.y/800.0, resolution ).x+0.02;
	col.g = tsample(GaussianBlurSampler,float2(x+scuv.x+0.0000,scuv.y-chroma_offs_y),resolution.y/800.0, resolution ).y+0.02;
	col.b = tsample(GaussianBlurSampler,float2(x+scuv.x-chroma_offs_x,scuv.y+0.0000),resolution.y/800.0, resolution ).z+0.02;
    
    float i = clamp(col.r*0.299 + col.g*0.587 + col.b*0.114, 0.0, 1.0 );
    i = pow( 1.0 - pow(i,2.0), 1.0 );
    i = (1.0-i) * 0.85 + 0.15; 
    
    /* Ghosting (toggleable) */
    float ghs = 0.15;
    if (ghosting_on) {
        float3 r = tsample(GaussianBlurSampler, float2(x-0.014*1.0, -0.027)*0.85+0.007*float2( 0.35*sin(1.0/7.0 + 15.0*uv.y + 0.9*time), 
            0.35*sin( 2.0/7.0 + 10.0*uv.y + 1.37*time) )+float2(scuv.x+0.001,scuv.y+0.001),
            5.5+1.3*sin( 3.0/9.0 + 31.0*uv.x + 1.70*time),resolution).xyz*float3(0.5,0.25,0.25);
        float3 g = tsample(GaussianBlurSampler, float2(x-0.019*1.0, -0.020)*0.85+0.007*float2( 0.35*cos(1.0/9.0 + 15.0*uv.y + 0.5*time), 
            0.35*sin( 2.0/9.0 + 10.0*uv.y + 1.50*time) )+float2(scuv.x+0.000,scuv.y-0.002),
            5.4+1.3*sin( 3.0/3.0 + 71.0*uv.x + 1.90*time),resolution).xyz*float3(0.25,0.5,0.25);
        float3 b = tsample(GaussianBlurSampler, float2(x-0.017*1.0, -0.003)*0.85+0.007*float2( 0.35*sin(2.0/3.0 + 15.0*uv.y + 0.7*time), 
            0.35*cos( 2.0/3.0 + 10.0*uv.y + 1.63*time) )+float2(scuv.x-0.002,scuv.y+0.000),
            5.3+1.3*sin( 3.0/7.0 + 91.0*uv.x + 1.65*time),resolution).xyz*float3(0.25,0.25,0.5);
			
        col += float3(ghs*(1.0-0.299),ghs*(1.0-0.299),ghs*(1.0-0.299))*pow(clamp(float3(3.0,3.0,3.0)*r,float3(0.0,0.0,0.0),float3(1.0,1.0,1.0)),float3(2.0,2.0,2.0))*float3(i,i,i);
        col += float3(ghs*(1.0-0.587),ghs*(1.0-0.587),ghs*(1.0-0.587))*pow(clamp(float3(3.0,3.0,3.0)*g,float3(0.0,0.0,0.0),float3(1.0,1.0,1.0)),float3(2.0,2.0,2.0))*float3(i,i,i);
        col += float3(ghs*(1.0-0.114),ghs*(1.0-0.114),ghs*(1.0-0.114))*pow(clamp(float3(3.0,3.0,3.0)*b,float3(0.0,0.0,0.0),float3(1.0,1.0,1.0)),float3(2.0,2.0,2.0))*float3(i,i,i);
    }
		
    /* Level adjustment (curves) with color mode selection */
    if (color_mode == 0) {
        // Natural mode - neutral gamma, no tint
        col.rgb = pow(col.rgb, gamma/2.2);
        col *= float3(1.0, 1.0, 1.0);
    } else if (color_mode == 1) {
        // Original mode - green tint WITH gamma control
        col.rgb = pow(col.rgb, gamma/2.2);  // Apply gamma from slider
        col *= float3(0.95, 1.05, 0.95);    // Then apply green tint
    } else if (color_mode == 2) {
        // NTSC Composite mode - YIQ-based color adjustment
        col.rgb = pow(col.rgb, gamma/2.2);  // Apply gamma from slider
        
        // RGB to YIQ conversion (NTSC standard)
        float Y = dot(col.rgb, float3(0.299, 0.587, 0.114));
        float I = dot(col.rgb, float3(0.595716, -0.274453, -0.321263));
        float Q = dot(col.rgb, float3(0.211456, -0.522591, 0.311135));
        
        // Apply NTSC adjustments (hardcoded values)
        const float ntsc_saturation = 1.05;  // Slightly boosted colors
        const float ntsc_hue_shift = -0.08;  // Warmer composite look
        
        I *= ntsc_saturation;
        Q *= ntsc_saturation;
        
        // Hue shift in YIQ space (rotate I/Q)
        float angle = ntsc_hue_shift * 3.14159;
        float cos_angle = cos(angle);
        float sin_angle = sin(angle);
        float I_shifted = I * cos_angle - Q * sin_angle;
        float Q_shifted = I * sin_angle + Q * cos_angle;
        
        // YIQ back to RGB
        col.r = Y + 0.956 * I_shifted + 0.621 * Q_shifted;
        col.g = Y - 0.272 * I_shifted - 0.647 * Q_shifted;
        col.b = Y - 1.106 * I_shifted + 1.703 * Q_shifted;
        
        // NTSC color emphasis (slight warmth typical of composite)
        col *= float3(1.02, 0.98, 0.96);
    }

    col = clamp(col*1.3 + 0.75*col*col + 1.25*col*col*col*col*col,float3(0.0,0.0,0.0),float3(10.0,10.0,10.0));
		
    /* Vignette (toggleable + aspect-aware) */
    if (vignette_on) {
        float vignette = 1.0;
        float2 vignetteUV = scuv; // Use straight UV
        
        if (FrameAspectMode == 1) {
            // Pillarbox mode (4:3 aspect)
            float aspect = resolution.x / resolution.y;
            float targetAspect = 4.0 / 2.99;
            float scale = aspect / targetAspect;
            float border = (1.0 - (1.0 / scale)) * 0.5;
            
            if (vignetteUV.x > border && vignetteUV.x < (1.0 - border)) {
                float vignetteX = (vignetteUV.x - border) / (1.0 - 2.0 * border);
                float vignetteY = vignetteUV.y;
                float vig = 16.0 * vignetteX * vignetteY * (1.0 - vignetteX) * (1.0 - vignetteY);
                vignette = 1.3 * pow(0.1 + vig, 0.5);
            }
        } else {
            // Wide mode (fullscreen)
            float vig = 16.0 * vignetteUV.x * vignetteUV.y * (1.0 - vignetteUV.x) * (1.0 - vignetteUV.y);
            vignette = 1.3 * pow(0.1 + vig, 0.5);
        }
        
        col *= vignette;
    }
    
    time *= scanroll;
		
    /* Scanlines - applied for modes 2 and above */
    if (CRTMode >= 2) {
        float scans = clamp( 0.35+0.18*sin(6.0*time-scuv.y*resolution.y*1.5), 0.0, 1.0);
        float s = pow(scans,0.9);
        col = col * float3(s,s,s);
    }

    /* Shadow Mask / Aperture Grille - applied based on mode */
    if (CRTMode == 3 || CRTMode == 4) {
        float maskU = uv_tx.x;
        float warpedPx = maskU * resolution.x;
        float stripeWidth = 3.0;
        float idx = warpedPx / stripeWidth;
        float center = frac(idx) - 0.5;
        float aa = fwidth(idx);
        float maskLine = saturate(1.0 - abs(center) / aa);
        
        if (CRTMode == 4) {
            // Aperture Grille - Brightness Lines (Trinitron-style)
            float3 brightnessMask = lerp(1.1, 0.8, maskLine);
            col.rgb *= brightnessMask;
        } else if (CRTMode == 3) {
            // Shadow Mask - RGB Color Stripes (standard CRT)
            float stripePhase = frac(idx);
            float3 phaseOffsets = float3(0.0, 1.0/3.0, 2.0/3.0);
            float3 rawDistance = abs(stripePhase.xxx - phaseOffsets);
            float3 circDistance = min(rawDistance, 1.0 - rawDistance);
            float3 maskLineRGB = saturate(1.0 - circDistance / aa);
            float3 colorMask = lerp(0.75, 1.5, maskLineRGB);
            col.rgb *= colorMask;
        }
    } else if (CRTMode == 1) {
        // Brightness matched - no effects but compensate for missing mask darkening
        col.rgb *= 0.60;
    }
		
    /* Tone map */
    col = filmic( col );
		
    /* Noise */
    float2 seed = scuv*resolution.xy;
    col -= 0.015*pow(float3(rand( seed +time ), rand( seed +time*2.0 ), rand( seed +time * 3.0 ) ), float3(1.5,1.5,1.5) );
		
    /* Flicker */
    col *= (1.0-0.004*(sin(50.0*time+uv.y*2.0)*0.5+0.5));

    return float4( col, 1.0 );
}

technique CRTSatPixie
{
	pass PS_CRTNewPixie_Accum
	{
		VertexShader=PostProcessVS;
		PixelShader=PS_NewPixie_Accum;
	}
	
	pass PS_CRTNewPixie_SaveBuffer {
		VertexShader=PostProcessVS;
		PixelShader=PrevColor;
		RenderTarget = tAccTex;
	}
	
	pass PS_CRTNewPixie_Blur
	{
		VertexShader = PostProcessVS;
		PixelShader = PS_NewPixie_Blur;
		RenderTarget = GaussianBlurTex;
	}
	
	pass PS_CRTNewPixie_Final
	{
		VertexShader = PostProcessVS;
		PixelShader = PS_NewPixie_Final;
	}
}