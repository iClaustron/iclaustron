# Architecture slides

A 13-slide deck, about eight minutes, presenting
[the architecture overview](../architecture.html): the threads and how
they can evolve, the data model, the asynchronous executor and the
synchronous models. Each slide has speaker notes.

The source is kept here: `deck.json` gives the title, the order of the
slides and the sections, and `slides/<id>.html` holds one slide each,
a 1920×1080 canvas with the notes in its `<aside>`. The slides use the
Slides format of claude.ai artifacts (custom elements such as
`<x-connector>` and `<x-shape>`), so they render in that viewer, which
also exports the deck as PDF or PPTX, and not as plain web pages.
