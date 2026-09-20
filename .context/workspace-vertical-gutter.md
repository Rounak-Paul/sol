# Workspace vertical gutter

The work surface begins one panel margin below the project-tabs strip. The
status bar owns the matching lower gutter through its top margin, so
`.workspace-main-content` must not add bottom padding. Its padding is `8px
8px 0px`: top and side space frame the floating panels, while the main work
area extends to the status-bar gutter.
