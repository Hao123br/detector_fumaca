idf_build_get_property(project_dir PROJECT_DIR)
idf_component_register(SRCS "pump_protection_main.c"
							"mqtt.c"
					INCLUDE_DIRS ".")
