/* FreeInkFont curated module list: TrueType (+GX variations) and CFF
 * OpenType (via sfnt + psaux), the smooth (anti-aliased) renderer, and
 * psnames. No autofit/Type1/BDF/etc. CFF is required so .otf (OTTO/CFF
 * outline) faces load — the stb backend renders them today, so dropping
 * them in the FreeType backend would be a regression. */
FT_USE_MODULE( FT_Module_Class, psnames_module_class )
FT_USE_MODULE( FT_Module_Class, psaux_module_class )
FT_USE_MODULE( FT_Module_Class, sfnt_module_class )
FT_USE_MODULE( FT_Driver_ClassRec, tt_driver_class )
FT_USE_MODULE( FT_Driver_ClassRec, cff_driver_class )
FT_USE_MODULE( FT_Renderer_Class, ft_smooth_renderer_class )
