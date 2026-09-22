# Illusionist native model

Measured 2026-09-22 on exe timestamp 0x6a85fbec, image size 0x448000.
Window: InGameUI+0x85378, primary vtable exe+0x31da48. Paulia's window
was visible in the live dev server, while text capture contained only Apply Illusion.

- Equipment boxes: pointer vector +0xa50/+0xa58; vt+0xa0 returns the item,
  object id at +0x30 (native input exe+0x28068e).
- Selected equipment +0x108, last equipment +0x10c; selected look +0x114,
  last look +0x118. Native input writes selection and calls exe+0x281640(this,true).
- All appearance pages: vector<vector<unsigned>> at +0x998/+0x9a0;
  page stride 24, ids stride 4, proven by page refresh exe+0x2823e0.
- Preview map sentinel +0x9c8, size +0x9d0: linked node equipment id +0x10,
  look object +0x18 (native Apply exe+0x2826e0).
- Cost text +0x1940, money +0x1848, number strings +0x40,
  written in native update exe+0x28031b / +0x2803d6.
- Apply button +0x598 through registry +0x948; listener at exe+0x280bb0
  receives this+0x90 and dispatches Apply to exe+0x2826e0.
- Window Show(false), exe+0x2810d0, is the normal close path.

The screen uses native state and callbacks, not text positions. Selections validate
membership against a fresh snapshot; Apply checks the staged item/look pairs,
cost, money and button enabled state again after a Cancel-first confirmation.
No paid Apply action is performed by development verification.

Read-only diagnostics: /illusionist. Live verification pending first build.
