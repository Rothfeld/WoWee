# Chat continuation lines

`ChatFrameTemplate` declares `<FontString indented="true"/>`. The FrameXML
emitter preserves this through `SetIndentedWordWrap`. Message rendering reserves
one font-height of horizontal space on continuation lines. The first line starts
at the frame's left edge; later lines wrap inside the remaining width.

Text measurement, shadow drawing, and hyperlink hit rectangles use the same
indentation. Each message starts a new paragraph. Markup colors and link payloads
survive line breaks.

Checks: `test_text_wrap` and `test_framexml`. Live screenshots and local-only
message injection script are in parent-workspace experiment
`_experiments/20261116T095227Z.chat_indentation`.
