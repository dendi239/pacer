#! /usr/bin/env python3

from pathlib import Path

import litgen


LITGEN_USE_NANOBIND = True


def my_litgen_options() -> litgen.LitgenOptions:
    # configure your options here
    options = litgen.LitgenOptions()
    options.bind_library = litgen.BindLibraryType.nanobind

    # ///////////////////////////////////////////////////////////////////
    #  Root namespace
    # ///////////////////////////////////////////////////////////////////
    # The namespace pacer is the C++ root namespace for the generated bindings
    # (i.e. no submodule will be generated for it in the python bindings)
    options.namespaces_root = ["pacer"]

    # //////////////////////////////////////////////////////////////////
    # Basic functions bindings
    # ////////////////////////////////////////////////////////////////////
    # No specific option is needed for these basic bindings
    # litgen will add the docstrings automatically in the python bindings

    # //////////////////////////////////////////////////////////////////
    # Classes and structs bindings
    # //////////////////////////////////////////////////////////////////
    # No specific option is needed for these bindings.
    # - Litgen will automatically add a default constructor with named parameters
    #   for structs that have no constructor defined in C++.
    #  - A class will publish only its public methods and members

    # ///////////////////////////////////////////////////////////////////
    #  Exclude functions and/or parameters from the bindings
    # ///////////////////////////////////////////////////////////////////
    # We want to exclude `inline void priv_SetOptions(bool v) {}` from the bindings
    # priv_ is a prefix for private functions that we don't want to expose
    options.fn_exclude_by_name__regex = "^priv_"

    # Inside `inline void SetOptions(bool v, bool priv_param = false) {}`,
    # we don't want to expose the private parameter priv_param
    # (it is possible since it has a default value)
    options.fn_params_exclude_names__regex = "^priv_"

    # ////////////////////////////////////////////////////////////////////
    # Override virtual methods in python
    # ////////////////////////////////////////////////////////////////////
    # The virtual methods of this class can be overriden in python
    options.class_override_virtual_methods_in_python__regex = "^RawGPSSource$"

    # ////////////////////////////////////////////////////////////////////
    # Publish bindings for template functions
    # ////////////////////////////////////////////////////////////////////
    #  template<typename T> T MaxValue(const std::vector<T>& values);
    # will be published as: max_value_int and max_value_float
    options.fn_template_options.add_specialization(
        "^MaxValue$", ["int", "float"], add_suffix_to_function_name=True
    )
    #  template<typename T> T MaxValue(const std::vector<T>& values);
    # will be published as: max_value_int and max_value_float
    options.fn_template_options.add_specialization(
        "^MinValue$", ["int", "float"], add_suffix_to_function_name=False
    )

    options.class_template_options.add_specialization("PointInTime", ["GPSSample"])

    options.class_template_options.add_ignore("VectorOperators")
    options.class_template_options.add_ignore("PointwiseOperators")
    options.class_template_options.add_ignore("LinearOperations")

    options.fn_template_options.add_ignore("Interpolate")
    options.fn_template_options.add_ignore("ToPoint")
    # options.fn_template_options.add_ignore("Split")

    # ////////////////////////////////////////////////////////////////////
    # Return values policy
    # ////////////////////////////////////////////////////////////////////
    # `Widget& GetWidgetSingleton()` returns a reference, that python should not free,
    # so we force the reference policy to be 'reference' instead of 'automatic'
    options.fn_return_force_policy_reference_for_references__regex = "Singleton$"

    # ////////////////////////////////////////////////////////////////////
    #  Boxed types
    # ////////////////////////////////////////////////////////////////////
    # Adaptation for `inline void SwitchBoolValue(bool &v) { v = !v; }`
    # SwitchBoolValue is a C++ function that takes a bool parameter by reference and changes its value
    # Since bool are immutable in python, we can to use a BoxedBool instead
    options.fn_params_replace_modifiable_immutable_by_boxed__regex = "^SwitchBoolValue$"

    # ////////////////////////////////////////////////////////////////////
    #  Published vectorized math functions and namespaces
    # ////////////////////////////////////////////////////////////////////
    # The functions in the MathFunctions namespace will be also published as vectorized functions
    options.fn_namespace_vectorize__regex = (
        r"^DaftLib::MathFunctions$"  # Do it in this namespace only
    )
    options.fn_vectorize__regex = r".*"  # For all functions

    # ////////////////////////////////////////////////////////////////////
    # Format the python stubs with black
    # ////////////////////////////////////////////////////////////////////
    # Set to True if you want the stub file to be formatted with black
    options.python_run_black_formatter = True

    return options


def autogenerate() -> None:
    repository_dir = Path(__file__).parent.parent

    header_files = [str(s) for s in (Path(repository_dir) / "pacer").glob("**/*.hpp")]
    header_files = [
        repository_dir / "pacer/datatypes/datatypes.hpp",
        repository_dir / "pacer/geometry/geometry.hpp",
        repository_dir / "pacer/laps/laps.hpp",
        repository_dir / "pacer/gps-source/gps-source.hpp",
        repository_dir / "pacer/laps-display/laps-display.hpp",
    ]

    output_cpp_pydef_file = repository_dir / "bindings/nanobind_pacer.cpp"

    litgen.write_generated_code_for_files(
        options=my_litgen_options(),
        input_cpp_header_files=[str(p) for p in header_files],
        output_cpp_pydef_file=output_cpp_pydef_file,
        output_stub_pyi_file=str(repository_dir / "bindings/pacer/__init__.pyi"),
    )


if __name__ == "__main__":
    autogenerate()
