idf_component_register(SRCS "stack_audit.c"
                       INCLUDE_DIRS "include"
                       REQUIRES freertos log)
# Debug builds only: idf.py -DFILEXFER_STACK_AUDIT=ON build (stays in the build
# dir's cache until -DFILEXFER_STACK_AUDIT=OFF)
if(FILEXFER_STACK_AUDIT)
  message(WARNING "Stack audit build: not for release")
  target_compile_definitions(${COMPONENT_LIB} PUBLIC FILEXFER_STACK_AUDIT=1)
endif()
