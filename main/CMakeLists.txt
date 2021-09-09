idf_component_register(
    SRCS "app.cpp"
    INCLUDE_DIRS ""
    EMBED_TXTFILES ../server_root_cert/store.pem
)

# set(COMPONENT_SRCS "app.cpp")
# set(COMPONENT_ADD_INCLUDEDIRS ".")
# set(COMPONENT_EMBED_TXTFILES "../server_root_cert/store.pem")

# register_component()
