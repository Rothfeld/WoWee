# Mirror timer stacking

Run widget regressions with `build/bin/test_widget_tree '[widget]'`.

Frame levels queried during XML OnLoad resolve from current parent relationships,
even before layout. MirrorTimer's status bar sets its level to its parent's level;
its fill then shares the region level with the label and border.

Within a shared frame level, draw layer, and sublevel, textures draw before font
strings. Creation order still resolves ties within either category. MirrorTimer
declares its border after its label, so creation order alone obscures the label's
ascenders. Native captures with the border shown and hidden establish this order.

Paired captures and the original FrameXML headless probe are stored in workspace
experiment `20261116T143220Z.swim_surface`.
