# The skeleton `fw new-app` copies. A display app and its main companion are
# ONE deliverable -- the main UF2 carries the display image inside it -- so
# they live in one folder and are declared by one file, display first.
add_executable(rf433_display display/main.c)
target_link_libraries(rf433_display fwog_display_bsp)
# stdio off UART0, linked at the bootloader's 0x10021000 app offset, and
# UF2/bin/map outputs. Every display app must call this.
fwog_display_app(rf433_display
    VERSION 001
    DESCRIPTION "Minimal display-CPU app skeleton: board_init then a DIAG heartbeat")

add_executable(rf433_main main/main.c)
target_link_libraries(rf433_main fwog_main_bsp)
# Carries the display firmware inside this UF2. With no argument it embeds
# this folder's own display app. Must come after target_link_libraries: the
# generated source needs fwog_main_bsp's include path.
fwog_embed_display_image(rf433_main)
fwog_main_app(rf433_main
    VERSION 001
    DESCRIPTION "Minimal main-CPU app skeleton: brings the display up, then a DIAG heartbeat")
