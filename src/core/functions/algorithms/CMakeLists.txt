set(EXTENSION_SOURCES
    ${EXTENSION_SOURCES}
    ${CMAKE_CURRENT_SOURCE_DIR}/graph_algorithm.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/strongly_connected_component.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/triangle_count.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/betweenness_centrality.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/label_propagation.cpp
    PARENT_SCOPE)
