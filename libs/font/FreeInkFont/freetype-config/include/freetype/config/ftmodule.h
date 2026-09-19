/* FreeInkFont curated module list: TrueType (+GX variations) and CFF
 * OpenType (via sfnt + psaux), the smooth (anti-aliased) renderer, and
 * psnames always; the classic B/W raster and the auto-hinter are opt-in
 * (FREEINK_FONT_ENABLE_MONOCHROME / FREEINK_FONT_ENABLE_AUTOHINT) since most
 * consumers render grayscale-AA only and never need FT_LOAD_FORCE_AUTOHINT.
 * No Type1/BDF/etc. CFF is required so .otf (OTTO/CFF outline) faces load —
 * the stb backend renders them today, so dropping them in the FreeType
 * backend would be a regression. */
FT_USE_MODULE( FT_Module_Class, psnames_module_class )
FT_USE_MODULE( FT_Module_Class, psaux_module_class )
FT_USE_MODULE( FT_Module_Class, sfnt_module_class )
FT_USE_MODULE( FT_Driver_ClassRec, tt_driver_class )
FT_USE_MODULE( FT_Driver_ClassRec, cff_driver_class )
FT_USE_MODULE( FT_Renderer_Class, ft_smooth_renderer_class )
#if FREEINK_FONT_ENABLE_MONOCHROME
FT_USE_MODULE( FT_Renderer_Class, ft_raster1_renderer_class )
#endif
#if FREEINK_FONT_ENABLE_AUTOHINT
FT_USE_MODULE( FT_Module_Class, autofit_module_class )
#endif
