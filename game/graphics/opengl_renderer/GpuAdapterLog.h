#pragma once

/*!
 * Boot log of the adapter the GL context runs on, as the system reports it: the LUID
 * (GL_EXT_memory_object_win32), its DXGI description, dedicated memory and PCI address, the
 * other adapters, the display the window opened on and the adapter driving it, and the driver's
 * free memory (GL_ATI_meminfo / GL_NVX_gpu_memory_info). Several adapters can share one model
 * name, so the GL_RENDERER string alone cannot tell boots apart. Call with the context current.
 */
void log_gpu_adapter();
