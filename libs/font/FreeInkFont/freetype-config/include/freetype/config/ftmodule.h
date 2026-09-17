/* FreeInkFont curated module list: TrueType (+GX variations) via sfnt, the
 * smooth (anti-aliased) renderer, and psnames. No autofit/CFF/Type1/BDF/etc. */
FT_USE_MODULE( FT_Module_Class, psnames_module_class )
FT_USE_MODULE( FT_Module_Class, sfnt_module_class )
FT_USE_MODULE( FT_Driver_ClassRec, tt_driver_class )
FT_USE_MODULE( FT_Renderer_Class, ft_smooth_renderer_class )
