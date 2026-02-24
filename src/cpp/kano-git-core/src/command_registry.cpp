// Command registry — wires up all subcommands



namespace kano::git::commands {

void RegisterAll(CLI::App& app) {
    RegisterVersion(app);

    // Smart tools (AI-powered)
    RegisterCommit(app);
    RegisterResolve(app);
    RegisterSync(app);
    RegisterPush(app);

    // Repository structure
    RegisterWorktree(app);
    RegisterSubtree(app);
    RegisterSubmodule(app);
    RegisterScalar(app);

    // VCS bridges
    RegisterP4(app);
    RegisterSvn(app);

    // Branch & workspace operations
    RegisterBranch(app);
    RegisterWorkspace(app);

    // Utilities
    RegisterClone(app);
    RegisterDoctor(app);
}

} // namespace kano::git::commands
