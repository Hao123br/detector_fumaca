idf_build_get_property(project_dir PROJECT_DIR)
idf_component_register(SRCS "detector_main.c"
							"mqtt.c"
					INCLUDE_DIRS ".")
